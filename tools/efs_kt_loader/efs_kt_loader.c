/*
 * efs_kt_loader - upload KT VoLTE EFS pack to modem via libdiag (CALLBACK_MODE).
 *
 * Mirrors a local directory tree onto modem EFS at /.  Handles regular EFS
 * files (diag EFS2) and NvItem__XXXXXXXX stubs (diag NV poke).
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>

#define LOG_TAG "efs_kt_loader"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define DIAG_SUBSYS_CMD_F 0x4b
#define DIAG_SUBSYS_FS 0x13
#define DIAG_NV_POKE_F 0x27

#define EFS2_DIAG_HELLO 0
#define EFS2_DIAG_OPEN 2
#define EFS2_DIAG_CLOSE 3
#define EFS2_DIAG_WRITE 5
#define EFS2_DIAG_MKDIR 9
#define EFS2_DIAG_STAT 15

#define O_WRONLY_EFS 1
#define O_CREAT_EFS 0x40
#define O_TRUNC_EFS 0x200
#define EFS_OPEN_FLAGS (O_WRONLY_EFS | O_CREAT_EFS | O_TRUNC_EFS)
#define EFS_FILE_MODE 0x81ff /* S_IFREG | 0777 */
#define EFS_DIR_MODE 0x41ff  /* S_IFDIR | 0777 */

#define EFS_CHUNK 1024
#define DIAG_TIMEOUT_MS 5000
#define NV_ITEM_DATA_LEN 128

#define VOLTE_EFS_SRC "/vendor/etc/volte-efs/kt"
#define VOLTE_KT_STAMP "/data/vendor/misc/volte_kt_efs.stamp"
#define VOLTE_KT_STAMP_VERSION "1"

#define CALLBACK_MODE 6
#define DIAG_PROC_MSM 0

typedef struct {
    int32_t diag_errno;
    int32_t mode;
    int32_t size;
    int32_t nlink;
    int32_t atime;
    int32_t mtime;
    int32_t ctime;
} efs_stat_rsp_t;

typedef struct {
    int32_t oflag;
    int32_t mode;
    char name[256];
} efs_open_req_t;

typedef struct {
    int32_t fd;
    int32_t diag_errno;
} efs_open_rsp_t;

typedef struct {
    int32_t fd;
    uint32_t offset;
    char data[EFS_CHUNK];
} efs_write_req_t;

typedef struct {
    int32_t fd;
    uint32_t offset;
    int32_t bytes_written;
    int32_t diag_errno;
} efs_write_rsp_t;

typedef struct {
    int16_t mode;
    char name[256];
} efs_mkdir_req_t;

typedef int (*diag_lsm_init_fn)(uint8_t *);
typedef int (*diag_lsm_deinit_fn)(void);
typedef int (*diag_send_data_fn)(unsigned char *, int);
typedef int (*diag_rx_cb_fn)(unsigned char *, int, void *);
typedef void (*diag_register_cb_fn)(diag_rx_cb_fn, void *);
typedef int (*diag_lsm_pkt_init_fn)(void);
typedef int (*diag_switch_logging_fn)(int, const char *);
typedef int (*diag_callback_send_fn)(int, unsigned char *, int);
typedef void *(*diagpkt_subsys_alloc_fn)(uint8_t, uint16_t, unsigned int);
typedef void (*diagpkt_commit_fn)(void *);

static void *g_libdiag;
static diag_lsm_init_fn g_diag_init;
static diag_lsm_deinit_fn g_diag_deinit;
static diag_send_data_fn g_diag_send;
static diag_register_cb_fn g_diag_register;
static diag_lsm_pkt_init_fn g_diag_pkt_init;
static diag_switch_logging_fn g_diag_switch_logging;
static diag_callback_send_fn g_diag_callback_send;
static diagpkt_subsys_alloc_fn g_diagpkt_subsys_alloc;
static diagpkt_commit_fn g_diagpkt_commit;
static uint8_t g_efs_subsys = DIAG_SUBSYS_FS;
static const uint8_t g_diag_proc = DIAG_PROC_MSM;
static int g_ok, g_fail;
static pthread_mutex_t g_rsp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rsp_cond = PTHREAD_COND_INITIALIZER;
static uint8_t g_rsp_buf[16384];
static size_t g_rsp_len;
static int g_rsp_ready;
static struct {
    const char *src_root;
    size_t src_root_len;
} g_walk;

static int diag_rsp_cb(unsigned char *data, int len, void *ctxt);
static void diag_drain_responses(int ms);
static void diag_cleanup(void);

static void str_copy(char *dst, size_t dst_len, const char *src) {
    snprintf(dst, dst_len, "%s", src);
}

static int diag_load_libdiag(void) {
    static const char *paths[] = {
        "/vendor/lib64/libdiag.so",
        "/system/vendor/lib64/libdiag.so",
        "libdiag.so",
        NULL,
    };

    for (int i = 0; paths[i]; i++) {
        g_libdiag = dlopen(paths[i], RTLD_NOW);
        if (g_libdiag)
            break;
    }
    if (!g_libdiag) {
        ALOGE("dlopen libdiag failed: %s", dlerror());
        return -1;
    }

    g_diag_init = (diag_lsm_init_fn)dlsym(g_libdiag, "Diag_LSM_Init");
    g_diag_deinit = (diag_lsm_deinit_fn)dlsym(g_libdiag, "Diag_LSM_DeInit");
    g_diag_send = (diag_send_data_fn)dlsym(g_libdiag, "diag_send_data");
    g_diag_register = (diag_register_cb_fn)dlsym(g_libdiag, "diag_register_callback");
    g_diag_pkt_init = (diag_lsm_pkt_init_fn)dlsym(g_libdiag, "Diag_LSM_Pkt_Init");
    g_diag_switch_logging = (diag_switch_logging_fn)dlsym(g_libdiag, "diag_switch_logging");
    g_diag_callback_send = (diag_callback_send_fn)dlsym(g_libdiag, "diag_callback_send_data");
    g_diagpkt_subsys_alloc = (diagpkt_subsys_alloc_fn)dlsym(g_libdiag, "diagpkt_subsys_alloc");
    g_diagpkt_commit = (diagpkt_commit_fn)dlsym(g_libdiag, "diagpkt_commit");
    if (!g_diag_init || (!g_diag_send && !g_diagpkt_commit)) {
        ALOGE("libdiag symbols missing");
        return -1;
    }

    if (!g_diag_init(NULL)) {
        ALOGE("Diag_LSM_Init failed");
        return -1;
    }

    if (g_diag_pkt_init && !g_diag_pkt_init()) {
        ALOGE("Diag_LSM_Pkt_Init failed");
        return -1;
    }

    if (g_diag_register)
        g_diag_register(diag_rsp_cb, NULL);

    if (g_diag_switch_logging)
        g_diag_switch_logging(CALLBACK_MODE, NULL);

    diag_drain_responses(500);

    ALOGI("diag LSM ready (pkt=%d switch=%d cb=%d send_cb=%d)",
          g_diagpkt_commit != NULL, g_diag_switch_logging != NULL, g_diag_register != NULL,
          g_diag_callback_send != NULL);
    return 0;
}

static void diag_drain_responses(int ms) {
    struct timespec end, now;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) {
        end.tv_sec++;
        end.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_rsp_lock);
    while (1) {
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
            break;
        struct timespec wait = end;
        g_rsp_ready = 0;
        pthread_cond_timedwait(&g_rsp_cond, &g_rsp_lock, &wait);
        g_rsp_ready = 0;
    }
    pthread_mutex_unlock(&g_rsp_lock);
}

static int diag_rsp_cb(unsigned char *data, int len, void *ctxt) {
    (void)ctxt;
    if (!data || len < 1)
        return 0;

    pthread_mutex_lock(&g_rsp_lock);
    g_rsp_len = len < (int)sizeof(g_rsp_buf) ? (size_t)len : sizeof(g_rsp_buf);
    memcpy(g_rsp_buf, data, g_rsp_len);
    g_rsp_ready = 1;
    pthread_cond_broadcast(&g_rsp_cond);
    pthread_mutex_unlock(&g_rsp_lock);
    return 0;
}

static int diag_send_and_wait(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap,
                              size_t *rsp_len) {
    pthread_mutex_lock(&g_rsp_lock);
    g_rsp_ready = 0;
    g_rsp_len = 0;
    pthread_mutex_unlock(&g_rsp_lock);

    if (g_diag_callback_send) {
        if (g_diag_callback_send(g_diag_proc, (unsigned char *)req, (int)req_len) < 0)
            return -1;
    } else if (g_diagpkt_subsys_alloc && g_diagpkt_commit && req_len >= 4 &&
               req[0] == DIAG_SUBSYS_CMD_F) {
        uint8_t subsys = req[1];
        uint16_t cmd = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
        size_t payload_len = req_len - 4;
        void *pkt = g_diagpkt_subsys_alloc(subsys, cmd, (unsigned int)payload_len);
        if (!pkt) {
            ALOGW("diagpkt_subsys_alloc failed subsys=0x%02x cmd=%u", subsys, cmd);
            return -1;
        }
        if (payload_len)
            memcpy(pkt, req + 4, payload_len);
        g_diagpkt_commit(pkt);
    } else if (g_diag_send) {
        if (g_diag_send((unsigned char *)req, (int)req_len) <= 0)
            return -1;
    } else {
        return -1;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += DIAG_TIMEOUT_MS / 1000;

    pthread_mutex_lock(&g_rsp_lock);
    while (1) {
        if (g_rsp_ready) {
            if (g_rsp_len >= 1 && g_rsp_buf[0] == 0x2f) {
                g_rsp_ready = 0;
            } else {
                break;
            }
        }
        if (pthread_cond_timedwait(&g_rsp_cond, &g_rsp_lock, &ts) != 0) {
            pthread_mutex_unlock(&g_rsp_lock);
            return -1;
        }
    }
    size_t blen = g_rsp_len;
    if (blen > rsp_cap)
        blen = rsp_cap;
    memcpy(rsp, g_rsp_buf, blen);
    *rsp_len = blen;
    g_rsp_ready = 0;
    pthread_mutex_unlock(&g_rsp_lock);
    return 0;
}

static int efs_cmd(uint8_t cmd, const void *req, size_t req_len, void *rsp, size_t rsp_len) {
    uint8_t pkt[16384];
    uint8_t out[16384];
    size_t out_len = 0;

    if (req_len + 4 > sizeof(pkt))
        return -1;

    pkt[0] = DIAG_SUBSYS_CMD_F;
    pkt[1] = g_efs_subsys;
    pkt[2] = cmd;
    pkt[3] = 0;
    if (req && req_len)
        memcpy(pkt + 4, req, req_len);

    if (diag_send_and_wait(pkt, req_len + 4, out, sizeof(out), &out_len) < 0) {
        ALOGW("diag_exchange failed cmd=%u subsys=0x%02x", cmd, g_efs_subsys);
        return -1;
    }
    if (out_len < 4) {
        ALOGW("short rsp len=%zu", out_len);
        return -1;
    }

    const uint8_t *body = out;
    size_t body_len = out_len;
    if (out[0] == DIAG_SUBSYS_CMD_F) {
        body = out + 4;
        body_len = out_len - 4;
    } else if (out[0] == 0x2f) {
        ALOGW("diag rejected cmd (0x2f)");
        return -1;
    }

    if (body_len < rsp_len)
        rsp_len = body_len;
    if (rsp && rsp_len)
        memcpy(rsp, body, rsp_len);
    return 0;
}

static int efs_wait_ready(void) {
    efs_stat_rsp_t st;
    for (int i = 0; i < 3; i++) {
        if (efs_cmd(EFS2_DIAG_STAT, "/", 2, &st, sizeof(st)) == 0) {
            ALOGI("EFS ready (subsys=0x%02x)", g_efs_subsys);
            return 0;
        }
        ALOGI("waiting for modem EFS (try %d/3)...", i + 1);
        sleep(1);
    }
    return -1;
}

static int efs_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strnlen(path, sizeof(tmp) - 1);
    if (len == 0 || path[0] != '/')
        return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        efs_mkdir_req_t req = {.mode = EFS_DIR_MODE};
        str_copy(req.name, sizeof(req.name), tmp);
        int32_t err = 0;
        efs_cmd(EFS2_DIAG_MKDIR, &req, 2 + strlen(req.name) + 1, &err, sizeof(err));
        *p = '/';
    }

    efs_mkdir_req_t req = {.mode = EFS_DIR_MODE};
    str_copy(req.name, sizeof(req.name), path);
    int32_t err = 0;
    efs_cmd(EFS2_DIAG_MKDIR, &req, 2 + strlen(req.name) + 1, &err, sizeof(err));
    return (err == 0) ? 0 : -1;
}

static int efs_put_file(const char *efs_path, const uint8_t *data, size_t size) {
    char parent[PATH_MAX];
    str_copy(parent, sizeof(parent), efs_path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        efs_mkdir_p(parent);
    }

    efs_open_req_t oreq = {
        .oflag = EFS_OPEN_FLAGS,
        .mode = EFS_FILE_MODE,
    };
    str_copy(oreq.name, sizeof(oreq.name), efs_path);
    efs_open_rsp_t orsp;
    if (efs_cmd(EFS2_DIAG_OPEN, &oreq, sizeof(oreq), &orsp, sizeof(orsp)) < 0)
        return -1;
    if (orsp.diag_errno != 0 || orsp.fd < 0) {
        ALOGE("EFS open %s errno=%d", efs_path, orsp.diag_errno);
        return -1;
    }

    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > EFS_CHUNK)
            chunk = EFS_CHUNK;

        efs_write_req_t wreq = {
            .fd = orsp.fd,
            .offset = (uint32_t)offset,
        };
        memcpy(wreq.data, data + offset, chunk);
        efs_write_rsp_t wrsp;
        if (efs_cmd(EFS2_DIAG_WRITE, &wreq, 8 + chunk, &wrsp, sizeof(wrsp)) < 0) {
            int32_t cerr = 0;
            efs_cmd(EFS2_DIAG_CLOSE, &orsp.fd, sizeof(int32_t), &cerr, sizeof(cerr));
            return -1;
        }
        if (wrsp.diag_errno != 0 || wrsp.bytes_written <= 0) {
            ALOGE("EFS write %s @%zu errno=%d", efs_path, offset, wrsp.diag_errno);
            int32_t cerr = 0;
            efs_cmd(EFS2_DIAG_CLOSE, &orsp.fd, sizeof(int32_t), &cerr, sizeof(cerr));
            return -1;
        }
        offset += (size_t)wrsp.bytes_written;
    }

    int32_t cerr = 0;
    efs_cmd(EFS2_DIAG_CLOSE, &orsp.fd, sizeof(int32_t), &cerr, sizeof(cerr));
    return 0;
}

static int nv_write_item(uint16_t item, const uint8_t *data, size_t len) {
    uint8_t req[4 + 2 + NV_ITEM_DATA_LEN + 2];
    uint8_t rsp[256];
    size_t rsp_len = 0;

    if (len > NV_ITEM_DATA_LEN)
        return -1;

    req[0] = DIAG_NV_POKE_F;
    req[1] = (uint8_t)(item & 0xff);
    req[2] = (uint8_t)(item >> 8);
    memset(req + 3, 0, NV_ITEM_DATA_LEN);
    memcpy(req + 3, data, len);
    req[3 + NV_ITEM_DATA_LEN] = 0;
    req[4 + NV_ITEM_DATA_LEN] = 0;

    if (diag_send_and_wait(req, sizeof(req), rsp, sizeof(rsp), &rsp_len) < 0)
        return -1;
    if (rsp_len < 1 || rsp[0] != DIAG_NV_POKE_F)
        return -1;
    return 0;
}

static int parse_nvitem_name(const char *name, uint16_t *item) {
    if (strncmp(name, "NvItem__", 8) != 0)
        return -1;
    const char *hex = name + 8;
    if (strlen(hex) != 8)
        return -1;
    unsigned int val = 0;
    if (sscanf(hex, "%8x", &val) != 1)
        return -1;
    *item = (uint16_t)val;
    return 0;
}

static int upload_file(const char *local_path, const char *efs_path, const char *base_name) {
    uint16_t nvitem;
    if (parse_nvitem_name(base_name, &nvitem) == 0) {
        int fd = open(local_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        uint8_t buf[NV_ITEM_DATA_LEN];
        ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n < 0)
            return -1;
        if (nv_write_item(nvitem, buf, (size_t)n) < 0) {
            ALOGE("NV poke item %u failed", nvitem);
            return -1;
        }
        ALOGI("NV %u (%zu bytes)", nvitem, (size_t)n);
        return 0;
    }

    int fd = open(local_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    uint8_t *data = NULL;
    size_t size = (size_t)st.st_size;
    if (size > 0) {
        data = malloc(size);
        if (!data) {
            close(fd);
            return -1;
        }
        size_t off = 0;
        while (off < size) {
            ssize_t n = read(fd, data + off, size - off);
            if (n <= 0) {
                free(data);
                close(fd);
                return -1;
            }
            off += (size_t)n;
        }
    }
    close(fd);

    int rc = efs_put_file(efs_path, data ? data : (const uint8_t *)"", size);
    free(data);
    if (rc == 0)
        ALOGI("EFS %s (%zu bytes)", efs_path, size);
    else
        ALOGE("EFS %s failed", efs_path);
    return rc;
}

static int walk_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;

    if (typeflag == FTW_DP)
        return 0;

    const char *rel = fpath + g_walk.src_root_len;
    while (*rel == '/')
        rel++;

    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;

    if (typeflag == FTW_D) {
        if (*rel == '\0')
            return 0;
        char efs_path[PATH_MAX];
        snprintf(efs_path, sizeof(efs_path), "/%s", rel);
        efs_mkdir_p(efs_path);
        return 0;
    }

    if (typeflag != FTW_F)
        return 0;

    if (*rel == '\0')
        return 0;

    char efs_path[PATH_MAX];
    snprintf(efs_path, sizeof(efs_path), "/%s", rel);

    if (upload_file(fpath, efs_path, base) == 0)
        g_ok++;
    else
        g_fail++;
    return 0;
}

static int stamp_matches(void) {
    int fd = open(VOLTE_KT_STAMP, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return strcmp(buf, VOLTE_KT_STAMP_VERSION) == 0;
}

static int write_stamp(void) {
    int fd = open(VOLTE_KT_STAMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        ALOGE("cannot write stamp %s: %s", VOLTE_KT_STAMP, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, VOLTE_KT_STAMP_VERSION "\n", strlen(VOLTE_KT_STAMP_VERSION) + 1);
    close(fd);
    return (n > 0) ? 0 : -1;
}

static int cmd_upload_tree(const char *src) {
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode)) {
        ALOGE("source %s is not a directory", src);
        return 1;
    }

    size_t root_len = strlen(src);
    while (root_len > 1 && src[root_len - 1] == '/')
        root_len--;

    g_walk.src_root = src;
    g_walk.src_root_len = root_len;
    char root_copy[PATH_MAX];
    snprintf(root_copy, sizeof(root_copy), "%.*s", (int)root_len, src);

    g_ok = 0;
    g_fail = 0;
    if (nftw(root_copy, walk_cb, 32, FTW_PHYS) < 0) {
        ALOGE("nftw failed: %s", strerror(errno));
        return 1;
    }

    ALOGI("upload done: ok=%d fail=%d", g_ok, g_fail);
    return g_fail ? 1 : 0;
}

static void diag_cleanup(void) {
    if (g_diag_deinit)
        g_diag_deinit();
    if (g_libdiag)
        dlclose(g_libdiag);
    g_libdiag = NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s install              One-shot ROM provisioning (stamp-gated)\n"
            "  %s upload <local-tree>  Mirror local tree to modem EFS /\n"
            "  %s probe                Test diag + EFS connectivity\n",
            prog, prog, prog);
}

static int cmd_probe(void) {
    efs_stat_rsp_t st;
    if (efs_cmd(EFS2_DIAG_STAT, "/", 2, &st, sizeof(st)) < 0) {
        ALOGE("EFS stat / failed");
        return 1;
    }
    ALOGI("EFS root stat ok, errno=%d mode=0%o size=%d", st.diag_errno, st.mode, st.size);
    return 0;
}

static int cmd_install(void) {
    if (stamp_matches()) {
        ALOGI("KT EFS already provisioned (stamp %s)", VOLTE_KT_STAMP_VERSION);
        return 0;
    }

    ALOGI("provisioning KT VoLTE EFS from %s", VOLTE_EFS_SRC);

    int rc = cmd_upload_tree(VOLTE_EFS_SRC);
    if (rc != 0) {
        ALOGE("first upload pass failed");
        return rc;
    }

    ALOGI("second upload pass (SONY VOLTE recommendation)");
    rc = cmd_upload_tree(VOLTE_EFS_SRC);
    if (rc != 0) {
        ALOGE("second upload pass failed");
        return rc;
    }

    if (write_stamp() < 0)
        return 1;

    ALOGI("KT VoLTE EFS provisioning complete");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (getuid() != 0)
        ALOGW("not running as root");

    if (diag_load_libdiag() < 0)
        return 1;

    if (efs_wait_ready() < 0) {
        ALOGE("EFS subsystem not reachable");
        diag_cleanup();
        return 1;
    }

    int rc = 1;
    if (!strcmp(argv[1], "probe")) {
        rc = cmd_probe();
    } else if (!strcmp(argv[1], "install")) {
        rc = cmd_install();
    } else if (!strcmp(argv[1], "upload") && argc >= 3) {
        if (cmd_probe() != 0) {
            ALOGE("probe failed before upload");
            rc = 1;
        } else {
            rc = cmd_upload_tree(argv[2]);
        }
    } else {
        usage(argv[0]);
        rc = 1;
    }

    diag_cleanup();
    return rc;
}

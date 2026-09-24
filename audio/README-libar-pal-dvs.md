# libar-pal — Cirrus DVS keep-haptic

Sau mọi **update/extract proprietary blobs**, bản vá DVS keep-haptic trên
`vendor/lib64/libar-pal.so` phải được áp lại (tránh stock extract ghi đè blob đã vá).

| Mục | Path |
|-----|------|
| **SSOT** | `audio/patch_libar_pal_dvs_keep_haptic.py` |
| **Hook extract** | `../extract-files.py` → `blob_fixup` trên `vendor/lib64/libar-pal.so` |
| **CLI** | `audio/patch-libar-pal-dvs-keep-haptic.sh` |

Không liên quan `patch-libar-pal-idempotent-custom-key.sh` (custom_key).

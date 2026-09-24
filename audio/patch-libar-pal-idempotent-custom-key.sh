#!/usr/bin/env bash
# Skip duplicate custom_key errors in Sony libar-pal.so without clearing the key.
# v3: branch to loop-continue (original tbz target), NOT success path that strlcpy("").
set -euo pipefail

SRC="${1:-vendor/sony/sm8550-common/proprietary/vendor/lib64/libar-pal.so}"
DST="${2:-}"

if [[ ! -f "$SRC" ]]; then
    echo "missing input: $SRC" >&2
    exit 1
fi

if [[ -z "$DST" ]]; then
    DST="${SRC%.so}.patched.so"
fi

cp -a "$SRC" "$DST"

python3 - "$DST" <<'PY'
import struct
import sys

path = sys.argv[1]

# tbz before ALOGE -> unconditional branch to loop continue (preserve existing key)
MISC_TBZ = {
    0x47f338: (0x47f3a8, "88030036"),
    0x47f3cc: (0x47f4a4, "c8060036"),
    0x47f4c8: (0x47f5a0, "c8060036"),
    0x47f5c4: (0x47f69c, "c8060036"),
    0x47f6c0: (0x47f798, "c8060036"),
    0x47f7d0: (0x47f8a8, "c8060036"),
    0x47f8e0: (0x47f9b8, "c8060036"),
    0x47f9f0: (0x47fac8, "c8060036"),
    0x47fb00: (0x47fbd8, "c8060036"),
    0x47fbfc: (0x47fcd4, "c8060036"),
    0x47fcf8: (0x47fdd0, "c8060036"),
    0x47fe08: (0x47fef0, "48070036"),
}

# game_vc: skip duplicate error log, continue without clearing
GAME_VC = {
    0x481a94: (0x481bbc, "e8024039"),
}


def encode_b(pc: int, tgt: int) -> bytes:
    off = (tgt - pc) // 4
    if not (-0x2000000 <= off < 0x2000000):
        raise ValueError(f"branch out of range pc=0x{pc:x} tgt=0x{tgt:x}")
    insn = (off & 0x3FFFFFF) | 0x14000000
    return struct.pack("<I", insn)


def apply(data: bytearray, table: dict) -> None:
    for pc, (tgt, expect) in table.items():
        old = data[pc:pc + 4]
        if old.hex() != expect:
            raise SystemExit(f"0x{pc:x}: expected {expect}, got {old.hex()}")
        patch = encode_b(pc, tgt)
        data[pc:pc + 4] = patch
        print(f"0x{pc:x}: {old.hex()} -> {patch.hex()} (->0x{tgt:x})")


with open(path, "r+b") as f:
    blob = bytearray(f.read())
    apply(blob, MISC_TBZ)
    apply(blob, GAME_VC)
    f.seek(0)
    f.write(blob)
    f.truncate()
PY

echo "patched -> $DST"
md5sum "$SRC" "$DST"

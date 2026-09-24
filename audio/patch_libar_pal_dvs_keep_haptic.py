#!/usr/bin/env python3
# Cirrus DVS keep-haptic tren libar-pal.so (SSOT).
#
# cirrus_dvs_update_devices @ VMA/file 0x48b4d0 (PT_LOAD identity map):
#   tbnz w24,#0,skip_remove -> b skip_remove
# de khong go PAL_DEVICE_OUT_HAPTICS khi stream type 22.
#
# Bat buoc ap lai sau moi update/extract proprietary blobs.
# extract-files.py blob_fixup goi blob_fixup_dvs_keep_haptic;
# patch-libar-pal-dvs-keep-haptic.sh goi module nay.
#
# Idempotent: da la `b` -> no-op; dung `tbnz` -> va; byte khac -> fail ro.

from __future__ import annotations

import sys
from pathlib import Path
from typing import Union

OFF = 0x48B4D0
OLD = 0x370008F8  # tbnz w24, #0, +0x11c
NEW = 0x14000047  # b +0x11c

DEFAULT_SRC = (
    'vendor/sony/sm8550-common/proprietary/vendor/lib64/libar-pal.so'
)


class PatchMismatchError(RuntimeError):
    """Offset khong phai tbnz stock hay b da va."""


def apply_dvs_keep_haptic(
    path: Union[str, Path],
    *,
    quiet: bool = False,
) -> str:
    """Ap DVS keep-haptic. Tra 'already' | 'patched'. Raise PatchMismatchError."""
    p = Path(path)
    if not p.is_file():
        raise PatchMismatchError(f'missing file: {p}')

    data = bytearray(p.read_bytes())
    if len(data) < OFF + 4:
        raise PatchMismatchError(
            f'{p}: file too short for offset 0x{OFF:x} (len={len(data)})'
        )

    word = int.from_bytes(data[OFF:OFF + 4], 'little')
    if word == NEW:
        msg = f'0x{OFF:x}: already patched ({word:#010x})'
        if not quiet:
            print(msg)
        return 'already'

    if word != OLD:
        raise PatchMismatchError(
            f'0x{OFF:x}: expected {OLD:#010x} (tbnz) or {NEW:#010x} (b), '
            f'got {word:#010x}'
        )

    data[OFF:OFF + 4] = NEW.to_bytes(4, 'little')
    p.write_bytes(data)
    msg = f'0x{OFF:x}: {OLD:#010x} -> {NEW:#010x} (keep haptic device)'
    if not quiet:
        print(msg)
    return 'patched'


def blob_fixup_dvs_keep_haptic(ctx, file, file_path, *args, **kwargs):
    """Entry cho blob_fixup().call() — vá blob vua extract tai file_path."""
    apply_dvs_keep_haptic(file_path)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    src = args[0] if args else DEFAULT_SRC
    try:
        apply_dvs_keep_haptic(src)
    except PatchMismatchError as e:
        print(str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

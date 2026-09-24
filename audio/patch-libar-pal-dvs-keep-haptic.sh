#!/usr/bin/env bash
# Cirrus DVS keep-haptic tren libar-pal.so (CLI wrapper -> SSOT Python).
# Bat buoc sau moi update/extract proprietary blobs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SSOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/patch_libar_pal_dvs_keep_haptic.py"
SRC="${1:-${ROOT}/vendor/sony/sm8550-common/proprietary/vendor/lib64/libar-pal.so}"
exec python3 "$SSOT" "$SRC"

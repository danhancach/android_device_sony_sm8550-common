#!/usr/bin/env bash
# Regenerate stock audio.primary.kalama.so with P2 speaker-safe routing fix.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
exec "$ROOT/.patchs/vendor/sony/sm8550-common/always-ahal-speaker-safe-binary.patch.sh" \
    "$ROOT/vendor/sony/sm8550-common/proprietary/vendor/lib64/hw/audio.primary.kalama.so"

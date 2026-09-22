#!/usr/bin/env bash
# Explicit real-device test: uses up to four attached NCS/NCS2, saves local evidence.
set -euo pipefail
qviewsr_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 1 || $# -gt 2 || ! -f "${1:-}" ]]; then
    echo "Usage: $0 /path/to/test-image.png [realHardwareGui|realScaleAndRepeatGui]" >&2
    exit 1
fi
qviewsr_assets=${QVIEWSR_ASSET_DIR:-"$qviewsr_repo/.local"}
if [[ ! -d "$qviewsr_assets/models/1032-fp16" ]]; then qviewsr_assets="$qviewsr_repo/../.local"; fi
export QVIEWSR_REAL_WORKER="$qviewsr_repo/build/worker/ncs-sr-worker"
export QVIEWSR_REAL_RUNTIME="$qviewsr_assets/openvino-2020.3.355/l_openvino_toolkit_runtime_ubuntu18_p_2020.3.355"
export QVIEWSR_REAL_MODEL="$qviewsr_assets/models/1032-fp16/single-image-super-resolution-1032.xml"
export QVIEWSR_REAL_IMAGE
QVIEWSR_REAL_IMAGE=$(realpath "$1")
export QVIEWSR_GUI_ARTIFACTS="${QVIEWSR_GUI_ARTIFACTS:-$qviewsr_repo/diagnostics/local/gui-check}"
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen}
exec "$qviewsr_repo/build/gui/tests/srtests" "${2:-realHardwareGui}"

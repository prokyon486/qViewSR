#!/usr/bin/env bash
# Real desktop/GPU tests; local models are read only and never uploaded.
set -euo pipefail
qviewsr_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -gt 1 || ( $# -eq 1 && ! -d "$1" ) ]]; then
    echo "Usage: $0 [directory-containing-glb-files]" >&2
    exit 1
fi
export QVIEWSR_TEST_3D=1
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
export QSG_RHI_BACKEND=${QSG_RHI_BACKEND:-opengl}
if [[ $# -eq 1 ]]; then export QVIEWSR_TEST_MODEL_DIR; QVIEWSR_TEST_MODEL_DIR=$(realpath "$1"); fi
export QVIEWSR_TEST_CAPTURE_DIR=${QVIEWSR_TEST_CAPTURE_DIR:-"$qviewsr_repo/diagnostics/local/glb-captures"}
exec env -u LD_LIBRARY_PATH "$qviewsr_repo/build/gui/tests/glbtests"

#!/usr/bin/env bash
set -euo pipefail
qviewsr_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! -x "$qviewsr_repo/build/gui/qviewsr" ]]; then
    echo "Build first: $qviewsr_repo/tools/build_qviewsr.sh" >&2
    exit 1
fi
# Old OpenVINO/OpenCV libraries are injected only into the child worker by the GUI.
exec env -u LD_LIBRARY_PATH "$qviewsr_repo/build/gui/qviewsr" "$@"

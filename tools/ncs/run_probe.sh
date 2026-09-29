#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 1 ]]; then
    echo 'Usage: bash tools/ncs/run_probe.sh /absolute/path/to/openvino-runtime-root [probe arguments]' >&2
    exit 64
fi
runtime_root=$(realpath "$1")
shift
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ie_root="$runtime_root/deployment_tools/inference_engine"
# Scoped to this child process. Never source old setupvars into the GUI environment.
export LD_LIBRARY_PATH="$ie_root/lib/intel64:$ie_root/external/tbb/lib:$runtime_root/deployment_tools/ngraph/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Bound firmware/model hangs; no forced USB reset is issued by this tool.
exec timeout --kill-after=5s 120s "$repo_root/build/ncs-probe/ncs-probe" "$@"

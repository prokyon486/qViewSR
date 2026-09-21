#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo 'Usage: bash tools/ncs/build_probe.sh /absolute/path/to/openvino-runtime-root' >&2
    exit 64
fi
runtime_root=$(realpath "$1")
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ie_root="$runtime_root/deployment_tools/inference_engine"
test -f "$ie_root/include/inference_engine.hpp"
test -f "$ie_root/lib/intel64/libmyriadPlugin.so"
mkdir -p "$repo_root/build/ncs-probe"
g++ -std=c++14 -O2 -Wno-deprecated-declarations \
    -I"$ie_root/include" "$repo_root/tools/ncs/probe.cpp" \
    -L"$ie_root/lib/intel64" \
    -Wl,-rpath-link,"$ie_root/external/tbb/lib" \
    -Wl,-rpath-link,"$runtime_root/deployment_tools/ngraph/lib" \
    -Wl,-rpath,"$ie_root/lib/intel64" \
    -linference_engine -linference_engine_legacy -o "$repo_root/build/ncs-probe/ncs-probe"
echo "$repo_root/build/ncs-probe/ncs-probe"

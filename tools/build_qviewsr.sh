#!/usr/bin/env bash
# Build the Qt 6 viewer and isolated OpenVINO worker on Ubuntu 24.04.
set -euo pipefail
qviewsr_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
qviewsr_assets=${QVIEWSR_ASSET_DIR:-"$qviewsr_repo/.local"}
if [[ ! -d "$qviewsr_assets/models/1032-fp16" && -d "$qviewsr_repo/../.local/models/1032-fp16" ]]; then
    qviewsr_assets="$qviewsr_repo/../.local"
fi
qviewsr_jobs=${QVIEWSR_BUILD_JOBS:-4}
for qviewsr_tool in cmake ninja g++ pkg-config python3; do
    if ! command -v "$qviewsr_tool" >/dev/null; then
        echo "Missing $qviewsr_tool. Run: sudo bash $qviewsr_repo/tools/install_build_deps.sh" >&2
        exit 1
    fi
done
python3 "$qviewsr_repo/tools/ncs/fetch_assets.py" --dest "$qviewsr_assets"
qviewsr_assets=$(realpath "$qviewsr_assets")
qviewsr_runtime="$qviewsr_assets/openvino-2020.3.355/l_openvino_toolkit_runtime_ubuntu18_p_2020.3.355"
cmake -S "$qviewsr_repo" -B "$qviewsr_repo/build/gui" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=ON -DQV_DISABLE_ONLINE_VERSION_CHECK=ON
cmake --build "$qviewsr_repo/build/gui" --parallel "$qviewsr_jobs"
qviewsr_ssl_args=()
# Optional local SDK used on the initial host; normal installs use libssl-dev.
qviewsr_sdk="$qviewsr_assets/sdk/usr/include"
if [[ ! -f /usr/include/openssl/ssl.h && -f "$qviewsr_sdk/openssl/ssl.h" ]]; then
    qviewsr_arch=$(gcc -dumpmachine)
    qviewsr_ssl_args=(-DOPENSSL_INCLUDE_DIR="$qviewsr_sdk"
        -DOPENSSL_CRYPTO_LIBRARY="$(gcc -print-file-name=libcrypto.so.3)"
        "-DCMAKE_CXX_FLAGS=-I$qviewsr_sdk/$qviewsr_arch")
fi
cmake -S "$qviewsr_repo/worker" -B "$qviewsr_repo/build/worker" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENVINO_ROOT="$qviewsr_runtime" "${qviewsr_ssl_args[@]}"
cmake --build "$qviewsr_repo/build/worker" --parallel "$qviewsr_jobs"
ctest --test-dir "$qviewsr_repo/build/worker" --output-on-failure
ctest --test-dir "$qviewsr_repo/build/gui" --output-on-failure
printf '\nReady: %s/tools/run_qviewsr.sh [image.png]\n' "$qviewsr_repo"

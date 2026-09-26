#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo 'Run with sudo in your terminal. Do not share your password.' >&2
    exit 1
fi
# Ubuntu 24.04 build dependencies. OpenVINO remains isolated in the workspace.
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 qt6-base-dev libqt6svg6-dev \
    qt6-image-formats-plugins qt6-l10n-tools qt6-translations-l10n libx11-dev liblcms2-dev libjpeg-dev libpng-dev libgif-dev \
    nlohmann-json3-dev libssl-dev
bash "$(dirname -- "$0")/install_3d_deps.sh"

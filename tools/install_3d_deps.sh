#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo 'Run with sudo in your terminal. Do not share your password.' >&2
    exit 1
fi
# Qt 6.4+ static glTF/GLB renderer and its QML runtime (Ubuntu 24.04).
apt-get update
apt-get install -y --no-install-recommends \
    qt6-declarative-dev qt6-quick3d-dev qt6-shadertools-dev \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    qml6-module-quick3d qml6-module-quick3d-assetutils qt6-quick3d-assetimporters-plugin

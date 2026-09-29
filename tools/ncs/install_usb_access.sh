#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo 'Run this script with sudo in your own terminal; do not share your password.' >&2
    exit 1
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
target=/etc/udev/rules.d/70-qviewsr-ncs.rules
if [[ -e "$target" ]] && ! cmp -s "$script_dir/70-qviewsr-ncs.rules" "$target"; then
    echo "Existing rule differs; review $target before replacing it." >&2
    exit 1
fi
install -m 0644 "$script_dir/70-qviewsr-ncs.rules" "$target"
udevadm control --reload-rules
udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=03e7
udevadm settle --timeout=10
echo 'NCS rules installed. If access remains unavailable, reconnect the NCS hub in your active desktop session.'

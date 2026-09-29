#!/usr/bin/env python3
"""Read Linux USB inventory without booting, resetting, or opening any VPU."""
import argparse
import datetime
import json
import os
from pathlib import Path

PRODUCTS = {"2150": "NCS (Myriad 2, unbooted)",
            "2485": "NCS2 (Myriad X, unbooted)",
            "f63b": "Myriad (booted; generation must be queried)"}


def inventory(sysfs=Path("/sys/bus/usb/devices"), devfs=Path("/dev/bus/usb")):
    devices = []
    for path in sorted(sysfs.iterdir()):
        def read(name):
            try:
                return (path / name).read_text().strip()
            except OSError:
                return None
        if read("idVendor") != "03e7":
            continue
        product = read("idProduct")
        bus, number = read("busnum"), read("devnum")
        node = devfs / f"{int(bus):03d}" / f"{int(number):03d}" if bus and number else None
        exists = node is not None and node.exists()
        devices.append({
            "usb_port": path.name, "vendor_id": "03e7", "product_id": product,
            "model_hint": PRODUCTS.get(product, "Unknown Intel/Movidius device"),
            "product": read("product"), "speed_mbps": read("speed"),
            "device_node": str(node) if node else None,
            "device_node_visible": exists,
            "read_write_access": bool(exists and os.access(node, os.R_OK | os.W_OK)),
            "status": "usb_enumerated_only", "inference_verified": False,
        })
    return {"schema_version": 1,
            "checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "devices": devices,
            "note": "USB enumeration does not prove firmware boot, model compilation, or inference."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(inventory(), ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result)
    else:
        print(result, end="")


if __name__ == "__main__":
    main()

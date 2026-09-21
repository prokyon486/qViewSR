#!/usr/bin/env python3
"""Fetch pinned official assets into a private workspace, with SHA-256 verification.

Requires Python 3.12+ for tar extraction filtering. Does not install system packages.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(asset, destination):
    path = destination / asset["file"]
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.stat().st_size == asset["size"] and digest(path) == asset["sha256"]:
            print(f"Verified existing: {path}")
            return path
        raise ValueError(f"Existing file differs from lock; move it aside before retrying: {path}")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as out:
            temporary = Path(out.name)
            with urllib.request.urlopen(asset["url"], timeout=60) as response:
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > asset["size"]:
                        raise ValueError(f"Download exceeds pinned size: {asset['url']}")
                    out.write(block)
        if temporary.stat().st_size != asset["size"] or digest(temporary) != asset["sha256"]:
            raise ValueError(f"Download failed size/SHA-256 verification: {asset['url']}")
        temporary.replace(path)
        print(f"Downloaded and verified: {path}")
        return path
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=repo / ".local")
    parser.add_argument("--models-only", action="store_true")
    args = parser.parse_args()
    destination = args.dest.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    lock = json.loads((repo / "docs/sr/source-lock.json").read_text())
    for asset in lock["model"]["files"]:
        fetch(asset, destination)
    if not args.models_only:
        runtime = lock["runtime"]
        archive = fetch(runtime, destination)
        target = destination / runtime["extract_directory"]
        if target.exists():
            print(f"Extraction directory already exists; left unchanged: {target}")
            print("Archive verified; existing extracted libraries are not re-verified.")
            return
        stage = Path(tempfile.mkdtemp(prefix="openvino-extract-", dir=destination))
        try:
            with tarfile.open(archive) as tar:
                tar.extractall(stage, filter="data")
            if not (stage / runtime["archive_root"] / "deployment_tools/inference_engine/include/inference_engine.hpp").is_file():
                raise ValueError("Runtime archive layout differs from lock")
            stage.rename(target)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
        print(f"Extracted: {target / runtime['archive_root']}")


if __name__ == "__main__":
    main()

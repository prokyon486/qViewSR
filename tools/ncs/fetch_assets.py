#!/usr/bin/env python3
"""Restore pinned assets from upstream, the owner's private archive, or local files.

Requires Python 3.12+ for tar extraction filtering. Does not install system packages.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(asset, destination, *, source="auto", archive=None, archive_dir=None):
    path = destination / asset["file"]
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.stat().st_size == asset["size"] and digest(path) == asset["sha256"]:
            print(f"Verified existing: {path}")
            return path
        raise ValueError(f"Existing file differs from lock; move it aside before retrying: {path}")
    sources = ["local"] if archive_dir else (["upstream", "github"] if source == "auto" else [source])
    failures = []
    for candidate in sources:
        try:
            with tempfile.TemporaryDirectory(prefix="fetch-", dir=path.parent) as stage:
                temporary = Path(stage) / path.name
                if candidate == "local":
                    shutil.copyfile(archive_dir / path.name, temporary)
                elif candidate == "github":
                    if not archive or not shutil.which("gh"):
                        raise RuntimeError("Private archive requires GitHub CLI (gh) and owner authentication")
                    subprocess.run(["gh", "release", "download", archive["tag"],
                                    "--repo", archive["repository"], "--pattern", path.name,
                                    "--dir", stage], check=True, timeout=600)
                else:
                    with temporary.open("wb") as out:
                        with urllib.request.urlopen(asset["url"], timeout=60) as response:
                            total = 0
                            while block := response.read(1024 * 1024):
                                total += len(block)
                                if total > asset["size"]:
                                    raise ValueError("Download exceeds pinned size")
                                out.write(block)
                if temporary.stat().st_size != asset["size"] or digest(temporary) != asset["sha256"]:
                    raise ValueError("Asset failed size/SHA-256 verification")
                temporary.replace(path)
                print(f"Restored and verified ({candidate}): {path}")
                return path
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            failures.append(f"{candidate}: {error}")
            print(f"Could not restore {path.name} from {candidate}: {error}")
    raise RuntimeError("; ".join(failures))


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=repo / ".local")
    parser.add_argument("--models-only", action="store_true")
    parser.add_argument("--source", choices=("auto", "upstream", "github"), default="auto")
    parser.add_argument("--archive-dir", type=Path,
                        help="Use downloaded release assets only; never access the network")
    args = parser.parse_args()
    destination = args.dest.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    lock = json.loads((repo / "docs/sr/source-lock.json").read_text())
    fetch_options = dict(source=args.source, archive=lock.get("preservation"), archive_dir=args.archive_dir)
    for asset in lock["model"]["files"]:
        fetch(asset, destination, **fetch_options)
    if not args.models_only:
        runtime = lock["runtime"]
        archive = fetch(runtime, destination, **fetch_options)
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

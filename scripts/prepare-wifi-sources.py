#!/usr/bin/env python3
"""Prepare project-owned, patched firmware source copies without changing upstream."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent


def prepare(source, patches):
    original = ROOT / "upstream" / source
    destination = ROOT / "outputs/esp32s31-ink-linux-sources" / source
    revision = subprocess.check_output(["git", "-C", str(original), "rev-parse", "HEAD"], text=True).strip()
    fingerprint = {"owner": "esp32s31-ink-linux-source-copy", "revision": revision,
                   "patches": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in patches}}
    if source == "esp-idf":
        # ESP-IDF truncates its app-description version to fit a 32-byte field.
        fingerprint["version"] = "v6.1.0-ink-linux-" + revision[:12]
    marker = destination / ".esp32s31-ink-linux-source.json"
    if destination.exists():
        if not marker.is_file() or json.loads(marker.read_text()).get("owner") != fingerprint["owner"]:
            raise ValueError(f"Refusing to replace an unowned source directory: {destination}")
        if json.loads(marker.read_text()) == fingerprint:
            print(f"Prepared: {destination}")
            return
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(original, destination, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "__pycache__"))
    # A source archive has no Git metadata; IDF's version file preserves provenance.
    if source == "esp-idf":
        (destination / "version.txt").write_text(fingerprint["version"] + "\n")
    for patch in patches:
        subprocess.run(["patch", "--forward", "--batch", "--fuzz=0", "-p1", "-d", str(destination),
                        "-i", str(patch)], check=True)
    marker.write_text(json.dumps(fingerprint, indent=2) + "\n")
    print(f"Prepared: {destination}")


def main():
    directory = ROOT / "platform/esp32s31/patches/esp-idf"
    patches = [directory / name for name in (
        "0001-rom-bindings.patch", "0002-mspi-underflow.patch",
        "0003-phy-clock-state.patch", "0004-timer-dispatch.patch",
        "0005-flash-suspend.patch")]
    prepare("esp-idf", patches)
    prepare("esp-hosted-linux", [])


if __name__ == "__main__":
    main()

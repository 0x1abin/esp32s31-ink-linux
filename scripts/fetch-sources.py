#!/usr/bin/env python3
"""Fetch or verify the exact upstream revisions in sources.lock.json."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="check local sources without fetching")
    parser.add_argument("--wifi", action="store_true", help="also fetch the pinned Wi-Fi firmware sources")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    lock = json.loads((root / "sources.lock.json").read_text())
    sources = dict(lock["sources"])
    if args.wifi:
        sources.update(lock["wifi_sources"])
    for name, source in sources.items():
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
            raise ValueError(f"Invalid source name: {name}")
        commit = source["commit"]
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError(f"Source {name} is not pinned to a full Git SHA")
        path = root / "upstream" / name
        if path.is_symlink():
            raise ValueError(f"Refusing a symlink checkout: {path}")
        if not (path / ".git").exists():
            if args.verify:
                raise ValueError(f"Missing source {name}; run scripts/fetch-sources.py")
            if path.exists() and any(path.iterdir()):
                raise ValueError(f"Refusing nonempty non-Git directory: {path}")
            path.mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "init", "-q", str(path)], check=True)
            git(path, "remote", "add", "origin", source["url"])
        if git(path, "status", "--porcelain"):
            raise ValueError(f"Source {name} has local changes; preserve them before fetching")
        try:
            current = git(path, "rev-parse", "--verify", "--quiet", "HEAD")
        except subprocess.CalledProcessError:
            current = ""
        if current != commit:
            if args.verify:
                raise ValueError(f"Source {name}: expected {commit}, found {current or 'no commit'}")
            git(path, "fetch", "--depth=1", source["url"], commit)
            git(path, "checkout", "--detach", commit)
        print(f"{name}: {commit}")
    if args.wifi:
        idf = root / "upstream/esp-idf"
        if not args.verify:
            subprocess.run(["git", "-C", str(idf), "submodule", "update", "--init", "--recursive"], check=True)
        status = git(idf, "submodule", "status", "--recursive")
        if any(line.startswith(("-", "+", "U")) for line in status.splitlines()):
            raise ValueError("ESP-IDF submodules are missing or do not match the pinned revision")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"fetch-sources: {error}", file=sys.stderr)
        sys.exit(1)

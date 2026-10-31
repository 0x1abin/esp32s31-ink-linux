#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Portable release metadata; keep the original inputs for build validation."""
import json
import re
from pathlib import Path


def public_config(data, root):
    text = data.decode()
    text = text.replace(str(root.resolve()), '${PROJECT_ROOT}')
    # External toolchains can live outside the checkout.
    text = re.sub(r'/(?:home|Users)/[^\s"\n]+', '${EXTERNAL_PATH}', text)
    return text.encode()


def firmware_metadata(description):
    return (json.dumps({key: description[key] for key in
                       ('project_name', 'project_version', 'target', 'git_revision')},
                      indent=2) + '\n').encode()


def check_public_bytes(data, root):
    roots = [str(root.resolve()).encode()]
    if str(Path.home()) not in ('/', '/root'):
        roots.append(str(Path.home()).encode())
    for prefix in roots:
        if prefix + b'/' in data:
            raise ValueError('Release contains a private build path')


def check_target(directory, root):
    # Inspect uncompressed payloads too; cramfs compression hides embedded paths.
    for path in directory.rglob('*'):
        if path.is_file() and not path.is_symlink():
            check_public_bytes(path.read_bytes(), root)

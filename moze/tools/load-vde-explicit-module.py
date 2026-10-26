#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Explicitly load a verified VDE module on its matching desktop kernel.

Keep the module outside depmod's autoload tree. The built-in experimental
driver stays opted out; no writable module parameter or bootloader change
is needed. This command does not install a service or alter boot files.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def trusted_file(path):
    path = path.resolve(strict=True)
    for node in [path, *path.parents]:
        info = node.stat()
        if info.st_uid != 0 or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise PermissionError(f'Root-owned, non-writable-by-others path required: {node}')
    if not path.is_file():
        raise ValueError('Expected a regular file')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run as root')
    module, record = trusted_file(args.module), trusted_file(args.record)
    metadata = json.loads(record.read_text())
    release = os.uname().release
    if release != '6.12.111-moze.1-native-vde2' or metadata['kernelrelease'] != release:
        raise ValueError('Only the separately qualified VDE2 release is accepted')
    requested = 'mocha_desktop_requested=1' in Path('/proc/cmdline').read_text().split()
    # A long press can enter multi-user.target from the charging target in
    # the same kernel. The root hardware loader records that transition;
    # the immutable boot command line cannot acquire a new desktop marker.
    marker = Path('/run/mocha-desktop-vde.requested')
    if not requested and marker.exists():
        info = marker.lstat()
        requested = (stat.S_ISREG(info.st_mode) and info.st_uid == 0
                     and not info.st_mode & 0o022 and marker.read_text() == release + '\n')
    if not requested:
        raise ValueError('Desktop boot marker required; do not load in charging mode')
    if hashlib.file_digest(module.open('rb'), 'sha256').hexdigest() != metadata['module_sha256']:
        raise ValueError('Module content does not match its build record')
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(module)], text=True).strip()
    if vermagic != metadata['vermagic'] or vermagic.split()[0] != release:
        raise ValueError('Module vermagic mismatch')
    device = Path('/sys/bus/platform/devices/60030000.video-codec')
    if b'nvidia,tegra124-vde' not in (device / 'of_node/compatible').read_bytes().split(b'\0'):
        raise ValueError('Expected Tegra124 VDE device')
    builtin_enabled = Path('/sys/module/tegra_vde/parameters/tegra124_enable').read_text().strip()
    if builtin_enabled != 'N':
        raise ValueError('The built-in Tegra124 probe must remain opted out')
    if (device / 'driver').exists() or Path('/sys/module/mocha_vde_candidate').exists():
        raise ValueError('VDE must be unbound and the explicit module unloaded')
    if args.check:
        print('VDE_EXPLICIT_MODULE_PRECONDITIONS_VERIFIED_NOT_LOADED')
        return
    subprocess.run(['insmod', str(module), 'tegra124_enable=1'], check=True, timeout=30)
    if (device / 'driver').resolve().name != 'mocha-vde-candidate':
        raise RuntimeError('Explicit VDE module did not bind')
    videos = [p for p in Path('/sys/class/video4linux').glob('video*')
              if device.resolve() in (p / 'device').resolve().parents
              or (p / 'device').resolve() == device.resolve()]
    if not videos:
        raise RuntimeError('No video device associated with VDE')
    print('VDE_EXPLICIT_MODULE_BOUND', ','.join(p.name for p in videos))


if __name__ == '__main__':
    main()

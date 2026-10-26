#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Load only release-matched bring-up modules after the data mount is ready.

Invoked by a temporary system service with --bundle and --release. A dedicated
command-line marker and exact kernel release guard keep the normal boot inert.
The persistent log distinguishes an early boot failure from missing networking.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--release', required=True)
    args = parser.parse_args()
    if (os.uname().release != args.release or
            'mocha_vde_diagnostic=1' not in Path('/proc/cmdline').read_text().split()):
        return
    if os.geteuid() != 0:
        raise SystemExit('Root is required for the guarded module preflight')
    bundle = args.bundle.resolve(strict=True)
    boot_id = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    logs = bundle / 'boot-diagnostics'
    logs.mkdir(exist_ok=True)
    with (logs / (boot_id + '.jsonl')).open('x', encoding='utf-8') as log:
        def record(event, **values):
            log.write(json.dumps(dict(event=event, monotonic=time.monotonic(), **values)) + '\n')
            log.flush()
            os.fsync(log.fileno())

        def run(command, timeout=25):
            record('command-start', command=command)
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
                record('command-end', command=command, returncode=result.returncode,
                       stdout=result.stdout[-3000:], stderr=result.stderr[-3000:])
                return result
            except subprocess.TimeoutExpired:
                record('command-timeout', command=command)
                return None

        record('root-reached', release=os.uname().release)
        module = bundle / 'mocha_miui_backlight.ko'
        entries = {}
        for line in (bundle / 'SHA256SUMS').read_text().splitlines():
            expected, name = line.split(maxsplit=1)
            entries[name.removeprefix('./')] = expected
        with module.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == entries[module.name]
        vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(module)], text=True)
        assert vermagic.split()[0] == args.release
        modules = Path('/lib/modules') / args.release
        assert modules.resolve() == bundle / 'modules/lib/modules' / args.release
        record('module-paths-verified')
        for policy in Path('/sys/devices/system/cpu/cpufreq').glob('policy*'):
            values = [int(v) for v in (policy / 'scaling_available_frequencies').read_text().split()]
            cap = max(v for v in values if v <= 696000)
            (policy / 'scaling_max_freq').write_text(str(cap))
            (policy / 'scaling_governor').write_text('powersave')
            record('cpu-bounded', max_khz=cap)
        if not Path('/sys/module/mocha_miui_backlight').exists():
            run(['insmod', str(module)])
        run(['modprobe', 'brcmfmac'])
        run(['udevadm', 'settle', '--timeout=15'], timeout=20)
        run(['nmcli', 'radio', 'wifi', 'on'])
        record('network-and-backlight-preflight-complete')


if __name__ == '__main__':
    main()

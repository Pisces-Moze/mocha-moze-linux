#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an isolated VDE development module for the running candidate kernel.

The built-in experimental driver must remain opted out and unbound. This
variant uses a separate driver and trace-event namespace and is loaded only
by an explicit insmod; this tool never installs or loads it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['source', 'output', 'moze', 'destination']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--jobs', type=int, default=2)
    args = p.parse_args()
    assert 1 <= args.jobs <= 4
    source, output, moze = (getattr(args, n).resolve(strict=True)
                            for n in ['source', 'output', 'moze'])
    destination = args.destination.resolve()
    assert not destination.exists(), 'Use a new independent module build directory'
    assert (output / 'Module.symvers').is_file()
    release = (output / 'include/config/kernel.release').read_text().strip()
    assert release.endswith(('-native-vde1', '-native-vde2')), 'Use a validated VDE candidate release'
    destination.mkdir(parents=True)
    directory = Path('drivers/media/platform/nvidia/tegra-vde')
    inputs = {}
    for name in ['vde.c', 'vde.h', 'h264.c', 'v4l2.c', 'tegra124-layout.h',
                 'iommu.c', 'dmabuf-cache.c', 'trace.h']:
        overlay = moze / 'source-overlay' / directory / name
        path = overlay if overlay.is_file() else source / directory / name
        data = path.read_bytes()
        inputs[name] = hashlib.sha256(data).hexdigest()
        text = data.decode()
        if name == 'vde.c':
            old = '.name\t\t= "tegra-vde"'
            assert text.count(old) == 1
            text = text.replace(old, '.name\t\t= "mocha-vde-candidate"')
        text = text.replace('trace_vde_', 'trace_mocha_vde_')
        if name == 'trace.h':
            text = text.replace('#define TRACE_SYSTEM tegra_vde',
                                '#define TRACE_SYSTEM mocha_vde_candidate')
            text = text.replace('#define TRACE_INCLUDE_PATH ../../drivers/media/platform/nvidia/tegra-vde',
                                '#define TRACE_INCLUDE_PATH .')
            for event in ['vde_writel', 'vde_readl', 'vde_setup_iram_entry',
                          'vde_ref_l0', 'vde_ref_l1', 'register_access']:
                text = re.sub(r'\b' + event + r'\b', 'mocha_' + event, text)
        (destination / name).write_text(text, newline='\n')
    (destination / 'Makefile').write_text('''# SPDX-License-Identifier: GPL-2.0-only
obj-m += mocha_vde_candidate.o
mocha_vde_candidate-y := vde.o iommu.o dmabuf-cache.o h264.o v4l2.o
ccflags-y += -I$(src)
''', newline='\n')
    command = ['make', '-C', str(source), 'O=' + str(output), 'ARCH=arm',
               'CROSS_COMPILE=arm-linux-gnueabihf-', 'LOCALVERSION=',
               'M=' + str(destination), '-j' + str(args.jobs), 'modules']
    subprocess.run(command, check=True)
    module = destination / 'mocha_vde_candidate.ko'
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(module)], text=True).strip()
    assert vermagic.split()[0] == release
    record = dict(kernelrelease=release, vermagic=vermagic, inputs_sha256=inputs,
                  module_sha256=hashlib.sha256(module.read_bytes()).hexdigest(),
                  driver_name='mocha-vde-candidate', loaded=False)
    (destination / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print('VDE_DEV_MODULE_BUILT_NOT_INSTALLED', record['module_sha256'])


if __name__ == '__main__':
    main()

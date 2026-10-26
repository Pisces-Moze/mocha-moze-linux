#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build unmodified UHID/uinput for the exact installed VDE2 kernel.

Does not install or load modules. Preserve the upstream source copyright
and licenses; input-compat.h retains the kernel's directory relationship.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
for name in ['source','output','destination']:
    p.add_argument('--'+name,type=Path,required=True)
args=p.parse_args()
release=(args.output/'include/config/kernel.release').read_text().strip()
if release!='6.12.111-moze.1-native-vde2':p.error('Use the matching installed VDE2 build output')
args.destination.mkdir(parents=True,exist_ok=False)
paths={'drivers/hid/uhid.c':'uhid.c','drivers/input/misc/uinput.c':'input/misc/uinput.c','drivers/input/input-compat.h':'input/input-compat.h'}
inputs={}
for original,relative in paths.items():
    data=(args.source/original).read_bytes();dest=args.destination/relative
    dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
    inputs[original]=hashlib.sha256(data).hexdigest()
(args.destination/'Makefile').write_text('obj-m += uhid.o\nobj-m += uinput.o\nuinput-y := input/misc/uinput.o\n',newline='\n')
subprocess.run(['make','-C',str(args.source.resolve()),'O='+str(args.output.resolve()),'M='+str(args.destination.resolve()),'ARCH=arm','CROSS_COMPILE=arm-linux-gnueabihf-','LOCALVERSION=','-j1','modules'],check=True)
record={'kernelrelease':release,'inputs_sha256':inputs,'modules':{},'installed':False,'loaded':False}
for name in ['uhid','uinput']:
    module=args.destination/(name+'.ko')
    magic=subprocess.check_output(['modinfo','-F','vermagic',str(module)],text=True).strip()
    assert magic.split()[0]==release
    record['modules'][module.name]={'sha256':hashlib.sha256(module.read_bytes()).hexdigest(),'vermagic':magic}
(args.destination/'build.json').write_text(json.dumps(record,indent=2)+'\n',newline='\n')
print(json.dumps(record))

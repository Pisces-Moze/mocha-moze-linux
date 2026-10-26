#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the isolated Mocha BCM serdev module; never installs or binds it.

Derives hci_bcm.c and serdev-only flow helpers from the matching Linux source,
retaining the upstream Intel copyright and GPL-2.0-or-later source header.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
for name in ['source','output','moze','destination']:
 parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args()
release=(args.output/'include/config/kernel.release').read_text().strip()
if release!='6.12.111-moze.1-native-vde2':parser.error('Use the matching installed VDE2 build output')
s=(args.moze/'source-overlay/drivers/bluetooth/hci_bcm.c').read_text()
assert s.count('bcmdev->drive_rts_on_open = true;')==1,'Mocha RTS fix is missing or changed'
# Isolate the same serdev implementation without registering another global
# HCI UART protocol or touching the built-in platform driver.
candidate=s.replace('"hci_uart_bcm"','"mocha_bcm_rts"')
anchor='\tstruct bcm_device *bcmdev;\n'
start=candidate.index('static int bcm_serdev_probe(');end=candidate.index('\nstatic void bcm_serdev_remove',start)
part=candidate[start:end];guard='\tstruct bcm_device *bcmdev;\n'
assert part.count(guard)==1
# The guard must follow declarations, so place it before the first allocation.
allocation='\tbcmdev = devm_kzalloc'
assert part.count(allocation)==1
part=part.replace(allocation,'\tif (!of_machine_is_compatible("nvidia,mocha") ||\n\t    !of_device_is_compatible(serdev->dev.of_node, "brcm,bcm43540-bt"))\n\t\treturn -ENODEV;\n\n'+allocation)
candidate=candidate[:start]+part+candidate[end:]
helpers='''
/* Serdev-only equivalents of the unexported hci_ldisc.c helpers. */
static void mocha_flow_control(struct hci_uart *hu, bool enable)
{
 serdev_device_set_flow_control(hu->serdev, !enable);
 serdev_device_set_rts(hu->serdev, !enable);
}
#define hci_uart_set_flow_control mocha_flow_control
#define hci_uart_has_flow_control(hu) ((hu)->serdev != NULL)
#define hci_uart_set_baudrate(hu, speed) serdev_device_set_baudrate((hu)->serdev, (speed))
'''
anchor='#include "hci_uart.h"\n';assert candidate.count(anchor)==1;candidate=candidate.replace(anchor,anchor+helpers)
candidate=candidate.replace('static struct platform_driver bcm_driver =','static struct platform_driver __maybe_unused bcm_driver =')
candidate=candidate.replace('MODULE_DEVICE_TABLE(of, bcm_bluetooth_of_match);','/* Explicit Mocha development module: no autoload aliases. */')
start=candidate.index('int __init bcm_init(void)')
candidate=candidate[:start]+'''static int __init mocha_bcm_init(void)
{
 return serdev_device_driver_register(&bcm_serdev_driver);
}
static void __exit mocha_bcm_exit(void)
{
 serdev_device_driver_unregister(&bcm_serdev_driver);
}
module_init(mocha_bcm_init);
module_exit(mocha_bcm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mocha BCM4354 RTS startup-order candidate");
'''

args.destination.mkdir(parents=True,exist_ok=False)
(args.destination/'mocha_bcm_rts.c').write_text(candidate,newline='\n')
for name in ['btbcm.h','hci_uart.h']:
 (args.destination/name).write_bytes((args.source/'drivers/bluetooth'/name).read_bytes())
(args.destination/'Makefile').write_text('obj-m += mocha_bcm_rts.o\n',newline='\n')
subprocess.run(['make','-C',str(args.source.resolve()),'O='+str(args.output.resolve()),'M='+str(args.destination.resolve()),'ARCH=arm','CROSS_COMPILE=arm-linux-gnueabihf-','LOCALVERSION=','-j1','modules'],check=True)
module=args.destination/'mocha_bcm_rts.ko'
vermagic=subprocess.check_output(['modinfo','-F','vermagic',str(module)],text=True).strip()
assert vermagic.split()[0]==release
record={'kernelrelease':release,'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'derived_source_sha256':hashlib.sha256(candidate.encode()).hexdigest(),'module_sha256':hashlib.sha256(module.read_bytes()).hexdigest(),'vermagic':vermagic,'installed':False,'loaded':False}
(args.destination/'build.json').write_text(json.dumps(record,indent=2)+'\n',newline='\n')
print(json.dumps(record))

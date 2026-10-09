#!/bin/bash
set -euo pipefail
export LOCALVERSION=
profile=${1:-stable}; out=$(realpath -m "${2:-../artifacts/kernel}")
tree=$(cd "$(dirname "$0")/../.." && pwd)
case "$profile" in stable) name=stable-desktop;; native) name=native-experimental;; *) echo 'Use stable or native';exit 2;; esac
mkdir -p "$out"
cp "$tree/moze/configs/$name.config" "$out/.config"
make -C "$tree" O="$out" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig
# COREDUMP alone does not enable ELF userspace core files on ARM.
for option in CONFIG_BINFMT_ELF CONFIG_COREDUMP CONFIG_ELF_CORE; do
 grep -qx "$option=y" "$out/.config" || { echo "Missing core-dump prerequisite: $option" >&2; exit 1; }
done
make -C "$tree" O="$out" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j"${JOBS:-4}" Image modules
make -C "$tree" O="$out" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH="$out/modules" INSTALL_MOD_STRIP=1 modules_install
dtc -I dts -O dtb -o "$out/mocha.dtb" "$tree/moze/dts/$name.dts"
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
 -n 'mocha moze linux 6.12.111-moze.1' -d "$out/arch/arm/boot/Image" "$out/uImage"
cp "$out/arch/arm/boot/Image" "$out/Image"
(cd "$out"; sha256sum Image uImage mocha.dtb .config > SHA256SUMS)
make -s -C "$tree" O="$out" ARCH=arm kernelrelease

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build a separate candidate. Never install it or select it as default boot.
set -eu
test "$#" -eq 4 || { echo "Usage: $0 SOURCE OUTPUT ACTIVE_CONFIG MOZE_DIR" >&2; exit 2; }
source=$(realpath "$1")
output=$(realpath "$2")
config=$(realpath "$3")
moze=$(realpath "$4")
test "$source" != "$output"
test -f "$source/Makefile"
test -f "$config"
test -f "$moze/dts/native-vde-experimental.dts"
test -f "$moze/dts/native-runtime.dts"
revision=${MOCHA_VDE_REVISION:-2}
case "$revision" in 1|2) ;; *) echo 'Unsupported candidate revision' >&2; exit 2;; esac
suffix=-moze.1-native-vde$revision
mkdir -p "$output"
cp "$config" "$output/.config"
"$source/scripts/config" --file "$output/.config" --set-str LOCALVERSION "$suffix"
"$source/scripts/config" --file "$output/.config" --disable LOCALVERSION_AUTO --module FUSE_FS
make -C "$source" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig
make -C "$source" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- prepare
release=$(make -s -C "$source" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- kernelrelease)
test "$release" = 6.12.111$suffix
make -C "$source" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j4 Image modules
make -C "$source" O="$output" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH="$output/module-root" modules_install
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
 -n 'Mocha VDE candidate' -d "$output/arch/arm/boot/Image" "$output/uImage-vde-candidate"
dtc -I dts -O dtb -i "$moze/dts" -o "$output/mocha-vde-candidate.dtb" "$moze/dts/native-vde-experimental.dts" 2>"$output/dtc-vde.log"
# Validate numeric provider references against the accepted decompiled base.
test "$(fdtget -t x "$output/mocha-vde-candidate.dtb" /clock@60006000 phandle)" = 3
test "$(fdtget -t x "$output/mocha-vde-candidate.dtb" /memory-controller@70019000 phandle)" = 4
test "$(fdtget -t x "$output/mocha-vde-candidate.dtb" /video-codec@60030000 iommus)" = '4 a'
test "$(fdtget -t x "$output/mocha-vde-candidate.dtb" /video-codec@60030000 resets)" = '3 3d 4 c 3 3f'
test "$(fdtget -t x "$output/mocha-vde-candidate.dtb" /iram@40000000/pool@10000 reg)" = '10000 10000'
(cd "$output"; sha256sum uImage-vde-candidate mocha-vde-candidate.dtb .config > VDE-SHA256SUMS)
printf 'VDE_CANDIDATE_BUILD_COMPLETE %s\n' "$release"

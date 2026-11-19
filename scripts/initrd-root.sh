#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Create $WORK/initrd-root, a small Ubuntu 26.04 arm64 root that the
# initramfs takes its storage tools from (initramfs/storage): cryptsetup,
# dmsetup, LVM, the ext4 tools and the fonts of the screens on the panel.
set -eu
. "$(dirname "$0")/env.sh"
R=$WORK/initrd-root
MIRROR=${UBUNTU_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITE=resolute
sudo rm -rf "$R"
sudo mmdebstrap --arch=arm64 --variant=essential --mode=root --components="main universe" \
	--include=cryptsetup-bin,dmsetup,lvm2,e2fsprogs,util-linux,fonts-ubuntu \
	"$SUITE" "$R" \
	"deb $MIRROR $SUITE main universe" \
	"deb $MIRROR $SUITE-updates main universe" \
	"deb $MIRROR $SUITE-security main universe"
echo "initramfs tools root $R"

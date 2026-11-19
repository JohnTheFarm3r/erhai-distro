#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Usage: build-initramfs.sh <static-arm64-busybox> <output.cpio.gz> [extra-root-dir [tools-root]]
# extra-root-dir is copied over the initramfs root (e.g. lib/firmware extracted from the device).
# tools-root is an Ubuntu arm64 root (scripts/initrd-root.sh) that the storage
# tools are taken from, with the libraries they load. ERHAI_UI is the built
# erhai-ui (initramfs/ui).
set -eu
bb=$1 out=$2 here=$(dirname "$(readlink -f "$0")")
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/bin" "$root/sbin" "$root/dev" "$root/proc" "$root/sys" "$root/run" "$root/tmp"
install -m 0755 "$bb" "$root/bin/busybox"
for applet in $(qemu-aarch64-static "$bb" --list 2>/dev/null || "$bb" --list); do
	[ "$applet" = busybox ] || ln -sf busybox "$root/bin/$applet"
done
install -m 0755 "$here/init" "$root/init"
install -m 0755 "$here/usbdiag" "$root/bin/usbdiag"
install -m 0755 "$here/usbtrace" "$root/bin/usbtrace"
install -m 0755 "$here/touchtest" "$root/bin/touchtest"
install -m 0755 "$here/ufscheck" "$root/bin/ufscheck"
install -m 0755 "$here/storage-ro" "$root/bin/storage-ro"
install -m 0755 "$here/storage" "$root/bin/storage"
mkdir -p "$root/etc/lvm"
install -m 0644 "$here/lvm.conf" "$root/etc/lvm/lvm.conf"
install -m 0644 "$here/keep" "$root/etc/erhai-keep"
[ -z "${ERHAI_UI:-}" ] || install -m 0755 "$ERHAI_UI" "$root/bin/erhai-ui"
${CROSS_COMPILE:-aarch64-linux-gnu-}gcc -static -Os -s -o "$root/bin/reboot-bootloader" "$here/reboot-bootloader.c"
if [ $# -ge 3 ]; then cp -a "$3"/. "$root"/; fi
if [ $# -ge 4 ]; then
	t=$4 readelf=${CROSS_COMPILE:-aarch64-linux-gnu-}readelf
	# Copy a file of the tools root to the same path, symlinks resolved.
	put() { mkdir -p "$root/$(dirname "$2")"; cp -L "$t/$1" "$root/$2"; }
	# Libraries are looked up in the multiarch directories and the RUNPATH.
	libs() {
		local rp n d
		rp=$($readelf -d "$1" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p' | sed 's|\$ORIGIN|'"${2%/*}"'|g')
		for n in $($readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
			for d in usr/lib/aarch64-linux-gnu $(echo "$rp" | tr : ' '); do
				d=${d#/}
				[ -e "$t/$d/$n" ] || continue
				[ -e "$root/$d/$n" ] || { put "$d/$n" "$d/$n"; libs "$root/$d/$n" "$d/$n"; }
				break
			done
		done
	}
	for f in usr/sbin/cryptsetup usr/sbin/dmsetup usr/sbin/lvm usr/sbin/mke2fs usr/sbin/resize2fs \
		usr/sbin/e2fsck usr/sbin/blkdiscard; do
		put $f $f; libs "$root/$f" $f
	done
	put usr/lib/ld-linux-aarch64.so.1 lib/ld-linux-aarch64.so.1
	put etc/mke2fs.conf etc/mke2fs.conf
	put usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf usr/share/fonts/erhai/Ubuntu.ttf
fi
(cd "$root" && find . | LC_ALL=C sort | cpio -o -H newc --owner 0:0 --reproducible --quiet) | gzip -9n > "$out"

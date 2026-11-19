#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build $OUT/initramfs.cpio.gz from initramfs/ with the staged firmware, the
# storage tools from $WORK/initrd-root (scripts/initrd-root.sh) and erhai-ui,
# the screens on the panel (initramfs/ui, built with LVGL).
set -eu
. "$(dirname "$0")/env.sh"
[ -x "$WORK/initrd-root/usr/sbin/lvm" ] || "$(dirname "$0")/initrd-root.sh"
LVGL=$WORK/src-cache/lvgl-9.2.2
[ -f "$LVGL/lvgl.h" ] || git clone -q --depth 1 -b v9.2.2 https://github.com/lvgl/lvgl.git "$LVGL"
make -s -C "$ROOT/initramfs/ui" -j"$(nproc)" CC="ccache ${CROSS_COMPILE}gcc" LVGL="$LVGL" \
	O="$WORK/erhai-ui" OUT="$WORK/erhai-ui/erhai-ui"
export ERHAI_UI=$WORK/erhai-ui/erhai-ui
"$ROOT/initramfs/build-initramfs.sh" "$BUSYBOX" "$OUT/initramfs.cpio.gz" "$FWROOT" "$WORK/initrd-root"

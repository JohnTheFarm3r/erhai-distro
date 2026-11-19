#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Configure and build the erhai-linux kernel, its DTB and modules.
set -eu
. "$(dirname "$0")/env.sh"
cd "$KERNEL_SRC"
mkdir -p "$KERNEL_OUT"
if [ ! -f "$KERNEL_OUT/.config" ]; then
	make -s ARCH=arm64 O="$KERNEL_OUT" defconfig
	scripts/kconfig/merge_config.sh -m -O "$KERNEL_OUT" "$KERNEL_OUT/.config" \
		arch/arm64/configs/erhai-firstboot.config \
		arch/arm64/configs/sm8750-peripherals.config \
		arch/arm64/configs/sm8750-netfilter.config \
		arch/arm64/configs/sm8750-snap.config \
		arch/arm64/configs/erhai-pad3.config
	make -s ARCH=arm64 O="$KERNEL_OUT" olddefconfig
fi
make -s ARCH=arm64 O="$KERNEL_OUT" LOCALVERSION= -j"$(nproc)" Image modules qcom/sm8750-erhai.dtb
rm -rf "$MODROOT"
make -s ARCH=arm64 O="$KERNEL_OUT" LOCALVERSION= INSTALL_MOD_PATH="$MODROOT" INSTALL_MOD_STRIP=1 modules_install
rm -f "$MODROOT"/lib/modules/*/build
ls -l "$KERNEL_OUT/arch/arm64/boot/Image" "$KERNEL_OUT/arch/arm64/boot/dts/qcom/sm8750-erhai.dtb"

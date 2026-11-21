#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage the Ubuntu root filesystem in $WORK/ubuntu-stage from the base system
# (scripts/ubuntu-base.sh): the erhai kernel modules, the tablet's firmware
# ($FWROOT, scripts/firmware.sh), the erhai hardware packages in $WORK/debs
# (scripts/debs.sh) and the overlay/ tree of this repository. There is no user
# account: GNOME's first run setup creates it on the tablet. Writes only to
# $WORK.
set -eu
. "$(dirname "$0")/env.sh"
S=$WORK/ubuntu-stage
rel=$(cat "$KERNEL_OUT/include/config/kernel.release")
ch() { sudo chroot "$S" "$@"; }

sudo rm -rf "$S"; sudo mkdir -p "$S"
sudo cp -a "$WORK/ubuntu-base"/. "$S"/

# Kernel modules and image (the boot images carry the kernel that boots; the
# copy in /boot keeps the installed system self-describing).
sudo rm -rf "$S/usr/lib/modules/$rel"; sudo mkdir -p "$S/usr/lib/modules"
sudo cp -a "$MODROOT/lib/modules/$rel" "$S/usr/lib/modules/"
sudo install -D -m 644 "$KERNEL_OUT/arch/arm64/boot/Image" "$S/boot/vmlinuz-$rel"

# Firmware and sensor configuration from the tablet's own partitions.
sudo cp -a --no-preserve=ownership "$FWROOT"/lib/firmware/. "$S"/usr/lib/firmware/
sudo cp -a --no-preserve=ownership "$FWROOT"/usr/. "$S"/usr/
sudo mkdir -p "$S/var/lib"
sudo cp -a --no-preserve=ownership "$FWROOT"/var/. "$S"/var/

# Hardware packages built for this tablet, installed from a local repository
# so APT knows them.
if ls "$WORK"/debs/*.deb >/dev/null 2>&1; then
	sudo rm -rf "$S/var/cache/erhai-debs"; sudo mkdir -p "$S/var/cache/erhai-debs"
	sudo cp "$WORK"/debs/*.deb "$S/var/cache/erhai-debs/"
	(cd "$S/var/cache/erhai-debs" && dpkg-scanpackages -m . /dev/null | sudo tee Packages >/dev/null)
	echo "deb [trusted=yes] file:/var/cache/erhai-debs ./" | sudo tee "$S/etc/apt/sources.list.d/erhai-local.list" >/dev/null
	sudo mount --bind /proc "$S/proc"; sudo mount --bind /dev "$S/dev"
	trap 'sudo umount "$S/dev" "$S/proc" 2>/dev/null || :' EXIT
	sudo cp /etc/resolv.conf "$S/etc/resolv.conf.erhai"
	ch sh -c 'mv /etc/resolv.conf /etc/resolv.conf.orig 2>/dev/null; cp /etc/resolv.conf.erhai /etc/resolv.conf'
	# APT's update and dpkg hooks (update notifier, command-not-found,
	# AppStream) only refresh caches the running system refreshes itself,
	# and take minutes in the build. Skip them while building.
	printf '#clear APT::Update::Post-Invoke-Success;\n#clear DPkg::Post-Invoke;\n' |
		sudo tee "$S/etc/apt/apt.conf.d/zz-erhai-build" >/dev/null
	ch apt-get -q update
	# The rebuilt versions of installed packages, plus packages/*/install.
	t=$(mktemp -d)
	sed -n 's/^Package: //p' "$S/var/cache/erhai-debs/Packages" | sort -u > "$t/built"
	{ ch dpkg-query -W -f '${Package}\n'; cat "$ROOT"/packages/*/install 2>/dev/null; } | sort -u > "$t/wanted"
	pkgs=$(comm -12 "$t/built" "$t/wanted" | tr '\n' ' '); rm -rf "$t"
	ch env DEBIAN_FRONTEND=noninteractive apt-get -q -y install --allow-downgrades $pkgs
	ch sh -c 'rm -f /etc/resolv.conf /etc/resolv.conf.erhai; mv /etc/resolv.conf.orig /etc/resolv.conf 2>/dev/null || :'
	ch apt-get -q clean
	sudo umount "$S/dev" "$S/proc"; trap - EXIT
	sudo rm -rf "$S/var/cache/erhai-debs" "$S/etc/apt/sources.list.d/erhai-local.list" \
		"$S/etc/apt/apt.conf.d/zz-erhai-build"
fi

sudo cp -a --no-preserve=ownership "$ROOT"/overlay/. "$S"/
# The default settings of the overlay (*.gschema.override).
ch glib-compile-schemas /usr/share/glib-2.0/schemas
# Face unlock: the face detection and recognition models
# (Apache-2.0), checked by hash, and its PAM profile in common-auth.
M=$WORK/face-models; mkdir -p "$M"
while read -r f sum; do
	n=${f##*/}
	[ -f "$M/$n" ] || curl -fsSL -o "$M/$n" "https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/$f"
	echo "$sum  $M/$n" | sha256sum -c --quiet
	sudo install -D -m 644 "$M/$n" "$S/usr/share/erhai-face/$n"
done <<EOF2
face_detection_yunet/face_detection_yunet_2023mar.onnx 8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4
face_recognition_sface/face_recognition_sface_2021dec.onnx 0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79
EOF2
ch env DEBIAN_FRONTEND=noninteractive pam-auth-update --enable erhai-face
# x86 programs: the x86-64 Ubuntu 24.04 system FEX runs them in, with the
# Adreno 830 Vulkan driver (scripts/fex-rootfs.sh). The OpenGL, Vulkan and
# Wayland calls of 64-bit programs go straight to the tablet's own libraries
# (fex-emu.json); 32-bit programs use the driver in that system. Both draw on
# the GPU.
sudo install -D -m 644 "$WORK/fex-rootfs/erhai-x86.sqsh" "$S/usr/share/fex-emu/RootFS/erhai-x86.sqsh"
sudo install -m 644 "$ROOT/tools/fex-emu.json" "$S/usr/share/fex-emu/Config.json"
# The sensor registry is written by the FastRPC server, which runs as the
# fastrpc user that its package created.
ch chown -R fastrpc:fastrpc /var/lib/hexagonrpcd

# System identity. No user account: with none, GDM starts GNOME's first run
# setup, which creates the owner's account (an administrator).
echo erhai | sudo tee "$S/etc/hostname" >/dev/null
printf '127.0.0.1\tlocalhost\n127.0.1.1\terhai\n::1\tlocalhost ip6-localhost ip6-loopback\n' | sudo tee "$S/etc/hosts" >/dev/null
# OpenCL on the GPU: Mesa offers it only for the drivers named here.
grep -q '^RUSTICL_ENABLE=' "$S/etc/environment" ||
	echo 'RUSTICL_ENABLE=freedreno' | sudo tee -a "$S/etc/environment" >/dev/null
: | sudo tee "$S/etc/fstab" >/dev/null
sudo rm -f "$S/etc/machine-id"; sudo touch "$S/etc/machine-id"
echo "staged $S"

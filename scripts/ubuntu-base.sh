#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bootstrap the Ubuntu Desktop 26.04 LTS (resolute) arm64 base system into
# $WORK/ubuntu-base, from the Ubuntu ports archive: the default GNOME desktop
# (ubuntu-desktop-minimal with its recommends), without a distribution kernel
# or boot loader, since the tablet boots the erhai kernel from its own boot
# images. Needs mmdebstrap and arm64 binfmt emulation on the build host.
set -eu
. "$(dirname "$0")/env.sh"
B=$WORK/ubuntu-base
IN="$0 $ROOT/overlay/etc/apt/sources.list.d/fex-emu.sources"
unchanged ubuntu-base "$B/usr/bin/FEX" $IN && exit 0
rm -f "$WORK/ubuntu-base.stamp"
MIRROR=${UBUNTU_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITE=resolute
# Packages that make no sense on this tablet: distribution kernels and
# firmware (the erhai kernel and the tablet's own firmware are installed
# instead), PC boot loaders, and the hardware enablement of other Qualcomm
# devices, which the software updater offers for any Snapdragon.
cat > "$WORK/ubuntu-pin.pref" <<EOF
Package: linux-image-* linux-modules-* linux-headers-* linux-generic* linux-firmware* grub-* shim-signed* flash-kernel hwe-qcom-* ubuntu-x1e-settings*
Pin: release *
Pin-Priority: -1
EOF
sudo rm -rf "$B"
sudo mmdebstrap --arch=arm64 --variant=apt --mode=root \
	--components="main restricted universe multiverse" \
	--aptopt='APT::Install-Recommends "true"' \
	--setup-hook='mkdir -p "$1/etc/apt/preferences.d"' \
	--setup-hook="cp '$WORK/ubuntu-pin.pref' \"\$1/etc/apt/preferences.d/erhai-no-pc-boot\"" \
	--include=ubuntu-minimal,ubuntu-standard,ubuntu-desktop-minimal,cryptsetup-bin,lsp-plugins-ladspa,gnome-snapshot,python3-opencv,gir1.2-gstreamer-1.0,gir1.2-gst-plugins-base-1.0,gstreamer1.0-plugins-bad \
	"$SUITE" "$B" \
	"deb $MIRROR $SUITE main restricted universe multiverse" \
	"deb $MIRROR $SUITE-updates main restricted universe multiverse" \
	"deb $MIRROR $SUITE-security main restricted universe multiverse"
# App Center and the snaps it needs, seeded the way Ubuntu's own images are:
# snapd installs them from /var/lib/snapd/seed on the first boot, offline.
SEED_SNAPS="snapd snap-store core24 bare gnome-46-2404 mesa-2404 gtk-common-themes"
sudo mkdir -p "$B/run/systemd/resolve"
sudo cp /etc/resolv.conf "$B/run/systemd/resolve/stub-resolv.conf"
sudo mount --bind /proc "$B/proc"; sudo mount --bind /dev "$B/dev"
trap 'sudo umount "$B/dev" "$B/proc" 2>/dev/null || :' EXIT
# FEX runs x86 and x86-64 programs, registered with binfmt_misc so they start
# like any other program.
sudo install -D -m 644 "$ROOT/overlay/etc/apt/sources.list.d/fex-emu.sources" \
	"$B/etc/apt/sources.list.d/fex-emu.sources"
sudo chroot "$B" sh -ec "
	apt-get -q update
	# Their install scripts restart systemd-binfmt; here that would reach the
	# build host's kernel. The tablet registers FEX at boot from binfmt.d.
	# APT runs them with its own search path, so it is given one with the
	# stand-in first.
	printf '#!/bin/sh\nexit 0\n' > /usr/local/sbin/service; chmod 755 /usr/local/sbin/service
	DEBIAN_FRONTEND=noninteractive apt-get -q -y -o DPkg::Path=/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin \\
		install fex-emu-armv8.4 fex-emu-binfmt32 fex-emu-binfmt64
	rm /usr/local/sbin/service
	# 32-bit programs keep their own Wayland client: 32-bit Vulkan has no
	# direct path to the tablet's driver, and the x86 system's driver needs the
	# x86 Wayland client. Without this file FEX leaves that library alone.
	dpkg-divert --rename --add --divert /usr/share/fex-emu/GuestThunks_32/libwayland-client-guest.so.disabled \
		/usr/share/fex-emu/GuestThunks_32/libwayland-client-guest.so
	apt-get -q clean"
sudo chroot "$B" sh -ec "
	t=\$(mktemp -d)
	snap known --remote model series=16 brand-id=generic model=generic-classic > \$t/model
	snap prepare-image --classic --arch=arm64 $(for s in $SEED_SNAPS; do printf -- '--snap %s ' $s; done) \$t/model \$t/root
	rm -rf /var/lib/snapd/seed
	mv \$t/root/var/lib/snapd/seed /var/lib/snapd/seed
	rm -rf \$t"
sudo umount "$B/dev" "$B/proc"; trap - EXIT
sudo rm -rf "$B/run/systemd/resolve"
finished ubuntu-base $IN
echo "bootstrapped $B"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build $WORK/fex-rootfs/erhai-x86.sqsh, the x86-64 Ubuntu 24.04 system FEX
# runs x86 programs in. It is the pinned FEX Ubuntu 24.04 image with the
# Vulkan driver for the Adreno 830 (Mesa 26.3.1, the release the system's own
# driver comes from) built for x86-64 and i386. 64-bit programs reach the tablet's own Vulkan
# driver directly; 32-bit programs have no such path and use this one.
# Needs unsquashfs and mksquashfs on an x86-64 build host.
set -eu
. "$(dirname "$0")/env.sh"
F=$WORK/fex-rootfs
OUTIMG=$F/erhai-x86.sqsh
BASE=Ubuntu_24_04.sqsh
BASE_URL=https://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/$BASE
BASE_SHA=2854b06d3ff1b8f6e526135bfb6dd5b7b30ab3ab73e79ae933a3d9fed959a178
MESA=mesa-26.3.1
MESA_URL=https://archive.mesa3d.org/$MESA.tar.xz
MESA_SHA=4e4e438eac7b840a9383988cbb7c08dad8041da544b9319fc26d319fa7348961
# One run at a time; a second one waits and then finds the result unchanged.
exec 9>"$WORK/fex-rootfs.lock"; flock 9
IN="$0"
unchanged fex-rootfs "$OUTIMG" $IN && exit 0
rm -f "$WORK/fex-rootfs.stamp"
mkdir -p "$F"

[ -f "$F/$BASE" ] || curl -fsSL -o "$F/$BASE" "$BASE_URL"
echo "$BASE_SHA  $F/$BASE" | sha256sum -c --quiet
[ -f "$F/$MESA.tar.xz" ] || curl -fsSL -o "$F/$MESA.tar.xz" "$MESA_URL"
echo "$MESA_SHA  $F/$MESA.tar.xz" | sha256sum -c --quiet

# The system as shipped, and a clean Ubuntu 24.04 to build in, with the same
# package sources, so the driver is built against the libraries it ships with.
T=$F/tree B=$F/build
sudo rm -rf "$T" "$B"
sudo unsquashfs -q -d "$T" "$F/$BASE" >/dev/null
sudo mmdebstrap --arch=amd64,i386 --variant=apt --mode=root \
	--setup-hook='mkdir -p "$1/etc/apt/sources.list.d"' \
	--setup-hook="cp '$T'/etc/apt/sources.list.d/*.sources \"\$1/etc/apt/sources.list.d/\"" \
	noble "$B" ""
sudo mount --bind /proc "$B/proc"; sudo mount --bind /dev "$B/dev"
sudo mount --bind /dev/pts "$B/dev/pts"
sudo cp /etc/resolv.conf "$B/etc/resolv.conf"
trap 'sudo umount "$B/dev/pts" "$B/dev" "$B/proc" 2>/dev/null || :' EXIT
sudo mkdir -p "$B/src"
sudo tar -C "$B/src" -xf "$F/$MESA.tar.xz"
sudo chroot "$B" sh -ec "
	export DEBIAN_FRONTEND=noninteractive
	apt-get -q update
	apt-get -q -y install build-essential gcc-multilib g++-multilib ninja-build pkg-config \
		python3-venv python3-mako python3-yaml python3-packaging bison flex glslang-tools \
		libdrm-dev libdrm-dev:i386 libexpat1-dev libexpat1-dev:i386 zlib1g-dev zlib1g-dev:i386 \
		libzstd-dev libzstd-dev:i386 libx11-xcb-dev libx11-xcb-dev:i386 libxcb-dri3-dev \
		libxcb-dri3-dev:i386 libxcb-present-dev libxcb-present-dev:i386 libxcb-sync-dev \
		libxcb-sync-dev:i386 libxcb-shm0-dev libxcb-shm0-dev:i386 libxcb-randr0-dev \
		libxcb-randr0-dev:i386 libxshmfence-dev libxshmfence-dev:i386 libxrandr-dev \
		libxrandr-dev:i386 libx11-dev libx11-dev:i386 libxext-dev libxext-dev:i386 \
		libxxf86vm-dev libxxf86vm-dev:i386 libxfixes-dev libxfixes-dev:i386 \
		libwayland-dev libwayland-dev:i386 wayland-protocols libwayland-bin
	python3 -m venv /opt/meson && /opt/meson/bin/pip -q install 'meson>=1.4,<1.9'
	cat > /src/i386.ini <<EOF
[binaries]
c = ['gcc', '-m32']
cpp = ['g++', '-m32']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
	cd /src/$MESA
	# wayland-protocols in Ubuntu 24.04 is older than Mesa needs; it is only
	# protocol descriptions, so take the one Mesa ships as a subproject.
	opts='--force-fallback-for=wayland-protocols -Dbuildtype=release -Db_ndebug=true -Dvulkan-drivers=freedreno -Dgallium-drivers=
		-Dfreedreno-kmds=msm -Dplatforms=x11,wayland -Dllvm=disabled -Dglx=disabled -Degl=disabled
		-Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs=
		-Dvulkan-layers= -Dtools= -Dbuild-tests=false -Dzstd=enabled'
	/opt/meson/bin/meson setup /src/b64 \$opts --libdir=lib/x86_64-linux-gnu
	/opt/meson/bin/meson setup /src/b32 \$opts --libdir=lib/i386-linux-gnu --cross-file /src/i386.ini
	ninja -C /src/b64 src/freedreno/vulkan/libvulkan_freedreno.so
	ninja -C /src/b32 src/freedreno/vulkan/libvulkan_freedreno.so
	strip --strip-unneeded /src/b64/src/freedreno/vulkan/libvulkan_freedreno.so
	strip --strip-unneeded /src/b32/src/freedreno/vulkan/libvulkan_freedreno.so"
sudo umount "$B/dev/pts" "$B/dev" "$B/proc"; trap - EXIT

# Into the shipped system: the same files the system's Vulkan driver package
# installed, now with the Adreno 830.
sudo install -m 644 "$B/src/b64/src/freedreno/vulkan/libvulkan_freedreno.so" \
	"$T/usr/lib/x86_64-linux-gnu/libvulkan_freedreno.so"
sudo install -m 644 "$B/src/b32/src/freedreno/vulkan/libvulkan_freedreno.so" \
	"$T/usr/lib/i386-linux-gnu/libvulkan_freedreno.so"
sudo rm -f "$OUTIMG"
sudo mksquashfs "$T" "$OUTIMG" -comp zstd -quiet -no-progress
sudo chown "$(id -u):$(id -g)" "$OUTIMG"
sudo rm -rf "$T" "$B"
finished fex-rootfs $IN
ls -l "$OUTIMG"

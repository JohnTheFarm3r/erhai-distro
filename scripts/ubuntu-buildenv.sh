#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Create $WORK/ubuntu-cross, an Ubuntu 26.04 amd64 build root with arm64 as a
# foreign architecture, for the erhai hardware packages (scripts/debs.sh).
# Packages are cross compiled for arm64 by the native toolchain, against the
# arm64 libraries of the same release.
set -eu
. "$(dirname "$0")/env.sh"
B=$WORK/ubuntu-cross
unchanged ubuntu-buildenv "$B/usr/bin/aarch64-linux-gnu-gcc" "$0" && exit 0
rm -f "$WORK/ubuntu-buildenv.stamp"
MIRROR=${UBUNTU_MIRROR:-http://archive.ubuntu.com/ubuntu}
PORTS=${UBUNTU_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITE=resolute
C="main restricted universe multiverse"
sudo rm -rf "$B"
sudo mmdebstrap --architectures=amd64,arm64 --variant=buildd --mode=root \
	--components="$C" \
	--include=build-essential,crossbuild-essential-arm64,devscripts,curl,equivs,fakeroot,ccache,quilt,dpkg-dev,ca-certificates,git \
	"$SUITE" "$B" \
	"deb [arch=amd64] $MIRROR $SUITE $C" \
	"deb [arch=amd64] $MIRROR $SUITE-updates $C" \
	"deb [arch=amd64] $MIRROR $SUITE-security $C" \
	"deb [arch=arm64] $PORTS $SUITE $C" \
	"deb [arch=arm64] $PORTS $SUITE-updates $C" \
	"deb [arch=arm64] $PORTS $SUITE-security $C" \
	"deb-src $MIRROR $SUITE $C" \
	"deb-src $MIRROR $SUITE-updates $C"
# The arm64 LLVM development files depend on llvm-N-tools, LLVM's own test
# utilities, which pull in arm64 Python and so cannot be installed next to the
# build tools. Nothing is built with them: empty packages of the same version
# stand in, from a local repository in the build root, so APT can bring them
# back whenever the arm64 LLVM is installed again.
sudo chroot "$B" sh -ec '
	apt-get -q update >/dev/null
	r=/opt/erhai-standins; mkdir -p $r; pin=
	for p in $(apt-cache pkgnames | grep -E "^llvm-[0-9]+-tools$"); do
		v=$(apt-cache show "$p:arm64" | sed -n "s/^Version: //p" | head -1)
		d=$(mktemp -d); cd "$d"
		printf "Section: devel\nPackage: %s\nVersion: %s\nArchitecture: arm64\nMulti-Arch: same\nDescription: empty stand-in for cross builds\n" "$p" "$v" > ctl
		equivs-build --arch=arm64 ctl >/dev/null 2>&1
		mv ./*.deb $r/; pin="$pin $p:arm64"
		cd /; rm -rf "$d"
	done
	cd $r && dpkg-scanpackages -m . /dev/null > Packages 2>/dev/null
	echo "deb [trusted=yes] file:$r ./" > /etc/apt/sources.list.d/erhai-standins.list
	# The archive copies of the stand-ins are never chosen for arm64.
	printf "Package:%s\nPin: release o=Ubuntu\nPin-Priority: -1\n" "$pin" > /etc/apt/preferences.d/erhai-standins
	apt-get -q update >/dev/null'
finished ubuntu-buildenv "$0"
echo "build root $B"

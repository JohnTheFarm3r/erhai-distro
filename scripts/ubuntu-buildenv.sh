#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Create $WORK/ubuntu-build, an Ubuntu 26.04 arm64 build root for the erhai
# hardware packages (scripts/debs.sh). It runs under arm64 binfmt emulation,
# so packages build exactly as they would on the tablet.
set -eu
. "$(dirname "$0")/env.sh"
B=$WORK/ubuntu-build
unchanged ubuntu-buildenv "$B/usr/bin/dpkg-buildpackage" "$0" && exit 0
rm -f "$WORK/ubuntu-buildenv.stamp"
MIRROR=${UBUNTU_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITE=resolute
sudo rm -rf "$B"
sudo mmdebstrap --arch=arm64 --variant=buildd --mode=root \
	--components="main restricted universe multiverse" \
	--include=build-essential,devscripts,curl,equivs,fakeroot,ccache,quilt,dpkg-dev,ca-certificates \
	"$SUITE" "$B" \
	"deb $MIRROR $SUITE main restricted universe multiverse" \
	"deb $MIRROR $SUITE-updates main restricted universe multiverse" \
	"deb $MIRROR $SUITE-security main restricted universe multiverse" \
	"deb-src $MIRROR $SUITE main restricted universe multiverse" \
	"deb-src $MIRROR $SUITE-updates main restricted universe multiverse"
finished ubuntu-buildenv "$0"
echo "build root $B"

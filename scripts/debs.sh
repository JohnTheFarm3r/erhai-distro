#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Cross compile the erhai hardware packages for arm64 as Ubuntu packages into
# $WORK/debs, in the build root of scripts/ubuntu-buildenv.sh. Each
# packages/<name>/ holds
#   source   "ubuntu SOURCE VERSION" (an Ubuntu 26.04 source package) or
#            "debian DSC-URL DSC-SHA256" (a pinned Debian source package),
#            "tarball URL SHA256 VERSION" or "git URL COMMIT VERSION" (an
#            upstream release, packaged with packages/<name>/debian), or
#            "local VERSION" (the source in packages/<name>/src, packaged
#            with packages/<name>/debian)
#   *.patch  applied on top as quilt patches, in name order
#   prepare  optional shell snippet run in the unpacked source tree
#   after    optional, the packages/ names this one is built against; they
#            are built first
# The version gets a +erhaiN suffix, so APT prefers it over the archive
# package of the same upstream version.
# A package whose recipe (packages/<name>/, and the recipes it is built
# against) is unchanged since its last build is not built again. The compiler
# cache lives in $WORK/ccache-debs, outside the build root, so it outlives a
# new build root.
#   usage: debs.sh [name...]   (default: every package)
#   ERHAI_REBUILD=1  build the packages even when unchanged
set -eu
. "$(dirname "$0")/env.sh"
B=$WORK/ubuntu-cross
D=$WORK/debs
REV=1
[ $# -gt 0 ] || set -- $(cd "$ROOT/packages" && ls)
# Build order: every package after the ones it is built against.
order= placed=
place() {
	case " $placed " in *" $1 "*) return ;; esac
	placed="$placed $1"
	[ -f "$ROOT/packages/$1/after" ] && for a in $(cat "$ROOT/packages/$1/after"); do place "$a"; done
	order="$order $1"
}
for n do place "$n"; done
set -- $order
# Fingerprint of a recipe and of the recipes it is built against.
stamp() {
	{ echo "rev $REV"; (cd "$ROOT/packages/$1" && find . -type f | LC_ALL=C sort | xargs sha256sum)
	  [ -f "$ROOT/packages/$1/after" ] && for a in $(cat "$ROOT/packages/$1/after"); do stamp "$a"; done; } |
		sha256sum | cut -d' ' -f1
}
built() { # the packages of the last build of $1 are all there, from this recipe
	[ -f "$D/$1.list" ] && [ "$(cat "$D/$1.stamp" 2>/dev/null)" = "$(stamp "$1")" ] || return 1
	while read -r f; do [ -f "$D/$f" ] || return 1; done < "$D/$1.list"
}
mkdir -p "$D"
sudo mkdir -p "$B/build"
sudo cp /etc/resolv.conf "$B/etc/resolv.conf"
# Mounts made here are undone on exit; ones already there belong to another
# run that is still building.
mounted=
mkdir -p "$WORK/ccache-debs"
for m in proc dev dev/pts erhai-debs var/cache/ccache; do
	mountpoint -q "$B/$m" && continue
	sudo mkdir -p "$B/$m"
	case $m in
	erhai-debs) sudo mount --bind "$D" "$B/$m" ;;
	var/cache/ccache) sudo mount --bind "$WORK/ccache-debs" "$B/$m" ;;
	*) sudo mount --bind "/$m" "$B/$m" ;;
	esac
	mounted="$m $mounted"
done
# No arm64 program may run during a build: arm64 emulation of the build
# machine is switched off while packages build and restored afterwards.
binfmt=/proc/sys/fs/binfmt_misc/qemu-aarch64
restore=
if [ -f $binfmt ] && grep -q enabled $binfmt; then
	echo 0 | sudo tee $binfmt >/dev/null; restore=1
fi
trap 'for m in $mounted; do sudo umount "$B/$m" 2>/dev/null || :; done
	[ -z "$restore" ] || echo 1 | sudo tee $binfmt >/dev/null' EXIT
# Packages built earlier satisfy the build dependencies of later ones.
echo "deb [trusted=yes] file:/erhai-debs ./" | sudo tee "$B/etc/apt/sources.list.d/erhai-debs.list" >/dev/null
index() { (cd "$D" && dpkg-scanpackages -m . /dev/null > Packages 2>/dev/null); sudo chroot "$B" apt-get -q update >/dev/null; }
index

for n do
	P=$ROOT/packages/$n
	if [ "${ERHAI_REBUILD:-0}" != 1 ] && built "$n"; then echo "$n: unchanged"; continue; fi
	set -- $(cat "$P/source")
	W=/build/$n
	sudo rm -rf "$B$W"; sudo mkdir -p "$B$W"
	case $1 in
	ubuntu) sudo chroot "$B" sh -c "cd $W && apt-get -q source --only-source $2=$3" ;;
	debian)
		f=${2##*/}
		sudo chroot "$B" sh -c "cd $W && dget -q -d -u '$2'"
		echo "$3  $B$W/$f" | sha256sum -c --quiet
		sudo chroot "$B" sh -c "cd $W && dpkg-source -q -x $f" ;;
	tarball|git)
		sp=$(sed -n 's/^Source: //p' "$P/debian/control")
		o=$B$W/${sp}_$4.orig.tar.gz
		if [ $1 = tarball ]; then
			sudo curl -fsSL -o "$o" "$2"
			echo "$3  $o" | sha256sum -c --quiet
		else
			t=$(mktemp -d); git -C "$t" init -q; git -C "$t" fetch -q --depth 1 "$2" "$3"
			git -C "$t" archive --format=tar --prefix="$sp-$4/" FETCH_HEAD | gzip -n | sudo tee "$o" >/dev/null
			rm -rf "$t"
		fi
		sudo mkdir -p "$B$W/$sp-$4"
		sudo tar -xzf "$o" -C "$B$W/$sp-$4" --strip-components=1
		sudo cp -r "$P/debian" "$B$W/$sp-$4/" ;;
	local)
		sp=$(sed -n 's/^Source: //p' "$P/debian/control")
		sudo mkdir -p "$B$W/$sp-$2"
		sudo cp -r "$P/src" "$P/debian" "$B$W/$sp-$2/" ;;
	esac
	src=$(cd "$B$W" && ls -d */ | head -1); src=${src%/}
	T=$B$W/$src
	sudo mkdir -p "$T/debian/patches"
	for p in "$P"/*.patch; do
		[ -e "$p" ] || continue
		sudo cp "$p" "$T/debian/patches/erhai-${p##*/}"
		echo "erhai-${p##*/}" | sudo tee -a "$T/debian/patches/series" >/dev/null
	done
	[ -f "$P/prepare" ] && sudo cp "$P/prepare" "$B$W/prepare" && sudo chroot "$B" sh -ec "cd $W/$src && . ../prepare"
	v=$(sudo chroot "$B" sh -c "cd $W/$src && dpkg-parsechangelog -S Version")
	sudo chroot "$B" sh -c "cd $W/$src && DEBEMAIL=erhai@localhost DEBFULLNAME=erhai \
		dch -b -v '$v+erhai$REV' -D resolute 'Hardware support for the OnePlus Pad 3.'"
	# dch renames the tree of a native package to the new version.
	src=$(cd "$B$W" && ls -d */ | head -1); src=${src%/}
	# A rebuilt dependency keeps its version, so APT would keep the copy
	# already in the build root: reinstall the ones installed there, all in
	# one go so that packages depending on each other upgrade together.
	re=
	[ -f "$P/after" ] && for a in $(cat "$P/after"); do
		while read -r f; do
			case $f in *_all.deb) pk=${f%%_*} ;; *) pk=${f%%_*}:arm64 ;; esac
			sudo chroot "$B" dpkg -s "$pk" >/dev/null 2>&1 && re="$re /erhai-debs/$f"
		done < "$D/$a.list"
	done
	[ -z "$re" ] || sudo chroot "$B" dpkg -i $re >/dev/null
	sudo chroot "$B" sh -c "cd $W/$src && apt-get build-dep -y -q --no-install-recommends -a arm64 -P cross,nocheck ./"
	# The compiler cache lives outside HOME, which the package build points
	# at a temporary directory.
	sudo chroot "$B" sh -c "cd $W/$src && DEB_BUILD_OPTIONS='nocheck noddebs parallel=$(nproc)' \
		CCACHE_DIR=/var/cache/ccache dpkg-buildpackage -a arm64 -Pcross,nocheck -b -uc -us" > "$WORK/debs-$n.log" 2>&1 || {
		echo "$n: build failed, see $WORK/debs-$n.log" >&2; exit 1; }
	rm -f "$D/$n".list
	for f in "$B$W"/*.deb; do cp "$f" "$D/"; echo "${f##*/}" >> "$D/$n.list"; done
	stamp "$n" > "$D/$n.stamp"
	echo "$n: $(wc -l < "$D/$n.list") packages"
	index
done

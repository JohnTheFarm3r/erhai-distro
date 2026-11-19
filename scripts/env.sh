# SPDX-License-Identifier: GPL-2.0-only
# Paths shared by the build scripts. Override any of them in the environment.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${WORK:=$HOME/erhai-work}"                 # all build output
: "${BACKUP:=$WORK/backup}"                   # partition images read from your own tablet
: "${SLOT:=a}"                                # the slot they were read from (fastboot getvar current-slot)
: "${KERNEL_SRC:=$HOME/erhai-linux}"          # erhai-linux checkout
: "${KERNEL_OUT:=$WORK/kernel-out}"
: "${MODROOT:=$WORK/modroot}"
: "${FWROOT:=$WORK/fwroot}"
: "${OUT:=$WORK/out}"
: "${MKBOOTIMG:=mkbootimg}"                   # AOSP mkbootimg
: "${AVBTOOL:=avbtool}"                       # AOSP avbtool
: "${AVB_KEY:=$WORK/keys/avb-boot-rsa4096.pem}"
: "${BUSYBOX:=$WORK/busybox}"                 # static arm64 busybox
: "${CROSS_COMPILE:=aarch64-linux-gnu-}"
export CROSS_COMPILE
# Kernel command line of the installed system.
CMDLINE="rdinit=/init cma=256M erhai.root=userdata erhai.timeout=0 erhai.maxtemp=0 panic=60 mem_sleep_default=s2idle fbcon=font:TER16x32 quiet loglevel=3 splash vt.global_cursor_default=0 systemd.show_status=auto rd.udev.log_level=3 systemd.gpt_auto=0"
mkdir -p "$WORK" "$OUT"
# The slow steps keep a fingerprint of their inputs next to their output and
# are skipped when nothing changed; ERHAI_REBUILD=1 runs them anyway.
#   unchanged NAME OUTPUT FILE...  true when OUTPUT exists from these FILEs
#   finished NAME FILE...          records the fingerprint after a run
fingerprint() { cat "$@" | sha256sum | cut -d' ' -f1; }
unchanged() {
	n=$1 o=$2; shift 2
	[ "${ERHAI_REBUILD:-0}" != 1 ] && [ -e "$o" ] &&
	[ "$(cat "$WORK/$n.stamp" 2>/dev/null)" = "$(fingerprint "$@")" ] &&
	echo "$n: unchanged"
}
finished() { n=$1; shift; fingerprint "$@" > "$WORK/$n.stamp"; }

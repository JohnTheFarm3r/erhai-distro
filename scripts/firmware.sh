#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage the firmware tree ($FWROOT/lib/firmware), the camera tuning
# ($FWROOT/usr/share/libcamera) and the sensor core files
# ($FWROOT/usr/share/qcom, $FWROOT/var/lib/hexagonrpcd) from the tablet's own
# partition images in $BACKUP (modem_$SLOT.img, bluetooth_$SLOT.img,
# super.img, dsp_$SLOT.img),
# plus the WCN7860 runtime firmware files and the audio
# topology built from source. Nothing proprietary is stored in this repo.
set -eu
. "$(dirname "$0")/env.sh"
SRC=$WORK/fw-src; FW=$FWROOT/lib/firmware
WIFI_FW_COMMIT=2b7207cfcb80c53c09dc6e04165789ecaf2d3348
WIFI_FW_SHA256=1c1db6a797f23a3d179904941e4a869679a514c6f43687e20954c5d96cad4f49
AR_COMMIT=993a17dcb672357998a463a73f120064d6c74f4f
AR_SHA256=58e0fa0ab9702c5237b4bea1f0486bcc8af7470fafd2d4ad8d508754fd269b17

fetch() { # url sha256 out
	[ -f "$3" ] || curl -fsSL -o "$3.part" "$1"
	[ -f "$3.part" ] && mv "$3.part" "$3"
	echo "$2  $3" | sha256sum -c --quiet
}

mkdir -p "$SRC"
for p in modem_$SLOT bluetooth_$SLOT; do
	[ -d "$SRC/$p" ] || { mkdir -p "$SRC/$p"; mcopy -s -n -i "$BACKUP/$p.img" ::image "$SRC/$p/"; }
done
for p in vendor_$SLOT odm_$SLOT; do
	if [ ! -d "$SRC/$p" ]; then
		python3 -I "$ROOT/tools/lpextract.py" "$BACKUP/super.img" "$p" "$SRC/$p.img"
		mkdir -p "$SRC/$p"; fsck.erofs --extract="$SRC/$p" "$SRC/$p.img" >/dev/null
		rm -f "$SRC/$p.img"
	fi
done
# DSP libraries and the sensor registry (ext4, read with debugfs).
extract() { # image path out
	[ -d "$3" ] && return
	rm -rf "$3.part"; mkdir -p "$3.part"
	debugfs -R "rdump $2 $3.part" "$1" 2>/dev/null
	[ -n "$(ls -A "$3.part/${2##*/}")" ] || { echo "$1: no $2" >&2; exit 1; }
	mv "$3.part" "$3"
}
extract "$BACKUP/dsp_$SLOT.img" /adsp "$SRC/dsp_$SLOT"
extract "$BACKUP/dsp_$SLOT.img" /cdsp "$SRC/dsp_cdsp_$SLOT"
fetch "https://github.com/ROCKNIX/extra-firmware/archive/$WIFI_FW_COMMIT.tar.gz" $WIFI_FW_SHA256 "$SRC/wifi-fw.tar.gz"
fetch "https://github.com/linux-msm/audioreach-topology/archive/$AR_COMMIT.tar.gz" $AR_SHA256 "$SRC/audioreach.tar.gz"

M=$SRC/modem_$SLOT/image; B=$SRC/bluetooth_$SLOT/image; V=$SRC/vendor_$SLOT; O=$SRC/odm_$SLOT
rm -rf "$FWROOT"
mkdir -p "$FW/qcom/sm8750/erhai" "$FW/qca" "$FW/awinic/erhai" "$FW/novatek" "$FW/ath12k/WCN7860/hw2.0"

# Remote processors (ADSP, CDSP, SoCCP), GPU and video.
cp "$M"/adsp.* "$M"/adsp_dtb.* "$M"/cdsp.* "$M"/cdsp_dtb.* "$M"/soccp.mbn "$FW/qcom/sm8750/erhai/"
cp "$V"/firmware/gen80000_zap.mbn "$V"/firmware/vpu35_4v.mbn "$FW/qcom/sm8750/erhai/"
cp "$V"/firmware/gen80000_sqe.fw "$V"/firmware/gen80000_gmu.bin "$V"/firmware/gen80000_aqe.fw "$FW/qcom/"
# Bluetooth.
cp "$B"/gngbtfw20.mbn "$B"/gngbtnv20.bin "$FW/qca/"
# Speaker amplifiers and touch panel.
cp "$O"/firmware/aw882xx_acf.bin "$FW/awinic/erhai/aw88261_acf.bin"
cp "$O"/firmware/tp/24924/FW_NT36536_HUAXING_FAE.img "$FW/novatek/nt36536-xn242.bin"

# Camera tuning for libcamera from the stock Chromatix files.
T=$FWROOT/usr/share/libcamera/ipa/softisp; mkdir -p "$T"
python3 -I "$ROOT/tools/chromatix-tuning.py" --af "$O"/lib64/camera/com.qti.tuned.erhaimain.bin "$T/sc1300cs.yaml"
python3 -I "$ROOT/tools/chromatix-tuning.py" "$O"/lib64/camera/com.qti.tuned.erhaifront.bin "$T/sc820cs.yaml"

# Sensor core on the ADSP: hexagonrpcd serves this tree to the sensors PD.
# The directory follows the device tree model (vendor, codename). The board
# configuration is used unmodified. The registry carries each tablet's own
# calibration: the tablet copies it from its persist partition on the first
# boot (erhai-sensor-registry), and the DSP writes it at runtime.
Q=$FWROOT/usr/share/qcom/sm8750/OnePlus/erhai
mkdir -p "$Q/dsp" "$Q/sensors" "$FWROOT/var/lib/hexagonrpcd"
cp -a "$SRC/dsp_$SLOT/adsp" "$Q/dsp/adsp"
# The compute DSP (NPU) process images for FastRPC clients, found through
# /usr/share/qcom/conf.d/erhai.yaml.
cp -a "$SRC/dsp_cdsp_$SLOT/cdsp" "$Q/dsp/cdsp"
cp -a "$V/etc/sensors/config" "$Q/sensors/config"
cp "$V/etc/sensors/sns_reg_config" "$Q/sensors/sns_reg.conf"
cp -a "$O/etc/sensor" "$Q/odm-sensor"
ln -s /var/lib/hexagonrpcd/sensors-registry "$Q/sensors/registry"
ln -s /sys/devices/soc0 "$Q/socinfo"

# WiFi: the runtime firmware files, board data from the tablet.
W=$FW/ath12k/WCN7860/hw2.0; t=$(mktemp -d)
tar -xzf "$SRC/wifi-fw.tar.gz" -C "$t" --strip-components=1 --wildcards '*/SM8750/ath12k/WCN7860/hw2.0/*'
for f in amss.bin m3.bin aux_ucode.bin qdss.cfg; do cp "$t/SM8750/ath12k/WCN7860/hw2.0/$f" "$W/"; done
rm -rf "$t"
cp "$O"/etc/wifi/peach/bdwlan.elf "$O"/etc/wifi/peach/regdb.bin "$W/"
python3 -I "$ROOT/tools/ath12k-bd.py" make "$W/board-2.bin" "$W/bdwlan.elf" "$W/regdb.bin" \
	'bus=pci,vendor=17cb,device=110e,subsystem-vendor=17cb,subsystem-device=110e,qmi-chip-id=2,qmi-board-id=255' \
	'bus=pci,qmi-chip-id=0,qmi-board-id=255'

# Audio topology.
t=$(mktemp -d)
tar -xzf "$SRC/audioreach.tar.gz" -C "$t" --strip-components=1
cp "$ROOT"/audio/audioreach/*.m4 "$t/audioreach/"
cp "$ROOT/audio/OnePlus-Pad-3.m4" "$t/"
(cd "$t" && m4 -I . OnePlus-Pad-3.m4 > OnePlus-Pad-3.conf && alsatplg -c OnePlus-Pad-3.conf -o OnePlus-Pad-3-tplg.bin)
cp "$t/OnePlus-Pad-3-tplg.bin" "$FW/qcom/sm8750/"
rm -rf "$t"
find "$FW" -type f | wc -l

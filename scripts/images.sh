#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the installable images in $OUT/install:
#   boot.img, init_boot.img, vendor_boot.img (header v4, AVB hash
#   footers with rollback index 0 like the stock images; boot is signed with
#   your own key, which an unlocked bootloader accepts),
#   root.img, the system as an ext4 image (what a system update writes over
#   the system volume, initramfs/storage), and
#   userdata.img, a sparse image of the userdata partition: the LVM volume
#   group "erhai" with only the system volume, root.img. On the first boot
#   the tablet asks what to encrypt, grows the system volume and creates the
#   volume for the owner's files (initramfs/storage).
# Writes only to $OUT. Set SKIP_ROOT=1 to rebuild only the boot images.
set -eu
. "$(dirname "$0")/env.sh"
I=$OUT/install; K=$KERNEL_OUT/arch/arm64/boot
mkdir -p "$I"
"$(dirname "$0")/initramfs.sh"
python3 -I "$ROOT/tools/mkabldtb.py" --mainline "$K/dts/qcom/sm8750-erhai.dtb" \
	--vendor-boot "$BACKUP/vendor_boot_$SLOT.img" --dtbo "$BACKUP/dtbo_$SLOT.img" --base-index 6 --out "$OUT/erhai-abl.dtb"
rm -f "$I"/boot.img "$I"/init_boot.img "$I"/vendor_boot.img
$MKBOOTIMG --header_version 4 --pagesize 4096 --kernel "$K/Image" --cmdline "$CMDLINE" --output "$I/boot.img"
t=$(mktemp -d); (cd "$t" && : | cpio -o -H newc 2>/dev/null) | gzip -n > "$I/empty.cpio.gz"; rmdir "$t"
$MKBOOTIMG --header_version 4 --pagesize 4096 --ramdisk "$I/empty.cpio.gz" --output "$I/init_boot.img"
$MKBOOTIMG --header_version 4 --pagesize 4096 --vendor_ramdisk "$OUT/initramfs.cpio.gz" \
	--dtb "$OUT/erhai-abl.dtb" --vendor_cmdline "" --base 0 --kernel_offset 0x00008000 \
	--ramdisk_offset 0x01000000 --tags_offset 0x00000100 --dtb_offset 0x01f00000 \
	--vendor_boot "$I/vendor_boot.img"
rm -f "$I/empty.cpio.gz"
salt() { sha256sum "$1" | cut -c1-64; }
$AVBTOOL add_hash_footer --image "$I/boot.img" --partition_name boot --partition_size 100663296 \
	--algorithm SHA256_RSA4096 --key "$AVB_KEY" --rollback_index 0 --salt "$(salt "$I/boot.img")"
$AVBTOOL add_hash_footer --image "$I/init_boot.img" --partition_name init_boot --partition_size 8388608 \
	--rollback_index 0 --salt "$(salt "$I/init_boot.img")"
$AVBTOOL add_hash_footer --image "$I/vendor_boot.img" --partition_name vendor_boot --partition_size 100663296 \
	--rollback_index 0 --salt "$(salt "$I/vendor_boot.img")"
if [ -z "${SKIP_ROOT:-}" ]; then
	# The system with a fifth of its size and 1 GiB free; it grows to the
	# volume on the tablet. The bootloader drops sparse chunks far into
	# userdata, so the images stay small.
	used=$(sudo du -sxb "$WORK/ubuntu-stage" | cut -f1)
	mb=$(((used * 12 / 10 / 1048576 + 1024 + 3) / 4 * 4))
	rm -f "$I/root.img" "$I/userdata.raw" "$I/userdata.img"
	sudo mke2fs -q -t ext4 -L erhai-root -J size=256 -O ^metadata_csum_seed -E root_owner=0:0 \
		-d "$WORK/ubuntu-stage" "$I/root.img" ${mb}M
	sudo chown "$(id -u):$(id -g)" "$I/root.img"
	e2fsck -fn "$I/root.img" >/dev/null
	# userdata: the volume group with the system volume, room for its metadata
	# in front. Only this loop device is scanned, so no volume of the build
	# host is touched.
	truncate -s $((mb + 8))M "$I/userdata.raw"
	loop=$(sudo losetup -f --show "$I/userdata.raw")
	trap 'sudo vgchange -q --devices "$loop" -an erhai 2>/dev/null; sudo losetup -d "$loop"' EXIT
	sudo pvcreate -q --devices "$loop" "$loop"
	sudo vgcreate -q --devices "$loop" -s 4M erhai "$loop"
	sudo lvcreate -q --devices "$loop" -y -Wn -Zn -L ${mb}M -n root erhai
	sudo dd if="$I/root.img" of=/dev/erhai/root bs=4M conv=sparse,fsync status=none
	sudo vgchange -q --devices "$loop" -an erhai
	sudo losetup -d "$loop"; trap - EXIT
	img2simg "$I/userdata.raw" "$I/userdata.img"; rm -f "$I/userdata.raw"
fi
for f in boot init_boot vendor_boot; do
	$AVBTOOL info_image --image "$I/$f.img" | grep -E '^(Image size|Rollback Index):' | tr '\n' ' '; echo "$f"
done
(cd "$I" && sha256sum *.img > SHA256SUMS && cat SHA256SUMS)

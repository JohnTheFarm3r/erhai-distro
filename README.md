# erhai-distro

Ubuntu Desktop 26.04 LTS (GNOME) for the OnePlus Pad 3 (OPD2415, codename
"erhai", Snapdragon 8 Elite SM8750), running on a mainline Linux kernel
kept in the separate `erhai-linux` repository. Android itself is not
touched: only the boot partitions (the kernel) and `userdata` are
replaced, and flashing the stock boot images back and formatting
`userdata` brings Android back.

This repository contains the build scripts, the initramfs, the Ubuntu
packages that add hardware support, and the system configuration. It
contains no proprietary files: the firmware and the stock device tree are
taken at build time from copies of your own tablet's partitions, made while
it still runs Android (see Building).

## Hardware support

| Component | Support |
|---|---|
| Display | 3392x2400 panel with backlight control |
| GPU | Adreno 830: OpenGL 4.6, OpenGL ES 3.2, Vulkan 1.4, OpenCL 3.0 |
| Touch and pen input | yes |
| Keyboard cover | keyboard and touchpad over the pogo connector |
| Buttons | power and volume |
| Wi-Fi | 2.4, 5 and 6 GHz |
| Bluetooth | yes |
| Speakers | all six, with thermal protection |
| Microphones | internal microphones |
| USB-C | USB 3, DisplayPort video and audio out, USB audio devices |
| Battery and charging | battery level, USB PD and PPS fast charging |
| Storage | system and personal files on separate volumes, optional encryption |
| Suspend | sleep and wake with the power button and the keyboard cover |
| Sensors | screen rotation, automatic brightness, compass, gyroscope |
| Cameras | front 8 MP and rear 13 MP with autofocus |
| Flashlight | from the quick settings |
| NPU | Hexagon NPU for programs that use FastRPC (`libcdsprpc`) |
| Video | hardware decoding of H.264, H.265, VP9 and AV1, encoding of H.264 and H.265 |
| Face unlock | with the front camera |
| x86 programs | run through FEX |

## Installing

Installing formats `userdata`, which erases all your data. The
bootloader must be unlocked. Only `userdata` and the boot partitions of the
current slot are written; firmware, bootloader and vbmeta partitions are
not touched. The current slot must be the one the partition copies for the
build were made from (see Building).

```
fastboot getvar current-slot
fastboot flash boot        boot.img
fastboot flash init_boot   init_boot.img
fastboot flash vendor_boot vendor_boot.img
fastboot flash userdata    userdata.img
fastboot reboot
```

On the first boot the tablet asks whether to encrypt the system and your
files, only your files, or nothing, and for a password. It then sets up the
storage, with the progress on the screen, and starts the GNOME setup, which
creates your account.

Do not relock the bootloader while this system is installed.

## Returning to Android

Flash the stock `boot`, `init_boot` and `vendor_boot` images of the current
slot from your copies, then erase the data with `fastboot -w`.

## Storage

The `userdata` partition holds two volumes:

| Volume | Size | Mounted on | Contents |
|---|---|---|---|
| `root` | 64 GiB | `/` | the system |
| `data` | the rest | `/data` | your files (`/home`) and the apps from the App Center and Flatpak |

| Choice at first boot | Encrypted | Password at startup |
|---|---|---|
| Encrypt everything (recommended) | `root` and `data` | yes |
| Encrypt only my files | `data` | yes |
| No encryption | nothing | no |

Encryption is LUKS2 (AES-256-XTS), done by the storage controller's inline
crypto engine. One password opens both volumes. To change it:

```
sudo cryptsetup luksChangeKey /dev/mapper/erhai-root
sudo cryptsetup luksChangeKey /dev/mapper/erhai-data
```

## System updates

There are two kinds of updates:

- **Ubuntu updates**: packages from Ubuntu's own repositories update as
  usual with the Software Updater or APT.
- **Releases of this build**: the hardware support (drivers, firmware, the
  packages and settings in this repository) comes as a new system image,
  `root.img`.

A release replaces the system volume and leaves `/data` alone. Copy the new
`root.img` to the tablet and run:

```
erhai-update root.img
```

The update is written on the next boot. Accounts, network, Bluetooth, face
unlock and the other settings listed in `initramfs/keep` are kept; more
paths can be added in `/data/system/keep.local`. Apps from the App Center
and Flatpak stay. Everything installed or updated with APT since the last
release is part of the system volume and is replaced with it: programs
installed with APT need to be installed again. A release with a new kernel
also comes with new boot images, flashed with fastboot as in Installing;
no data is lost.

Releases are planned to move to a package repository of their own, so they
install with APT next to Ubuntu's updates and nothing is replaced.

## Face unlock

Set up a face in Settings, Users, Face Unlock. It unlocks the lock screen
and answers sudo and administrator prompts. The password always works as
well, and the first login after a boot always asks for it.

The front camera has no depth sensor, so a photo of your face can unlock
the tablet.

## Troubleshooting

If the fastboot screen goes blank after `userdata` is flashed and
`fastboot reboot` gets no answer, hold the power button to restart. The
installation is complete.

If the tablet stops in fastboot after a reboot, check
`fastboot getvar slot-unbootable:a` (or `:b` for slot B). If it reports
`yes`, run `fastboot set_active a` (or `b`) and reboot.

## Building

The hardware packages (`scripts/debs.sh`) are cross compiled for arm64 by
the native toolchain, in an amd64 build root with arm64 as a foreign
architecture (`scripts/ubuntu-buildenv.sh`).

Requirements on the build machine: an aarch64 cross compiler, `mmdebstrap`,
`lvm2`, `git`, QEMU 10 or newer registered as the arm64 binfmt handler (with
the F flag), `dpkg-dev`, AOSP `mkbootimg` and `avbtool`, `img2simg`,
`mtools`, `erofs-utils`, `m4`, `alsa-utils`, `e2fsprogs`, `curl` and a
static arm64 busybox.

1. While the tablet still runs Android, with root access, find its slot
   with `adb shell getprop ro.boot.slot_suffix` (`_a` or `_b`) and copy
   these partitions of that slot into `$BACKUP`: `modem`, `bluetooth`,
   `vendor_boot`, `dtbo` and `dsp`, plus `super`. For slot A:

   ```
   adb exec-out "su -c 'dd if=/dev/block/by-name/modem_a bs=4M'" > $BACKUP/modem_a.img
   ```

   Also copy `boot` and `init_boot` of the slot the same way. The build
   does not use them, but together with `vendor_boot` they are what you
   need to return to Android. Set `SLOT=b` for the build if the copies are
   from slot B.
2. Create a key for signing the boot images:
   `openssl genrsa -out $WORK/keys/avb-boot-rsa4096.pem 4096`.
3. Run, in this order:

   ```
   scripts/kernel.sh
   scripts/firmware.sh
   scripts/ubuntu-buildenv.sh
   scripts/initrd-root.sh
   scripts/debs.sh
   scripts/ubuntu-base.sh
   scripts/fex-rootfs.sh
   scripts/ubuntu-rootfs.sh
   scripts/images.sh
   ```

The images are written to `$OUT/install`. Paths are set in
`scripts/env.sh` and can be overridden in the environment. Steps whose
inputs did not change are skipped; `ERHAI_REBUILD=1` builds everything
again.

## Repository layout

| Path | Contents |
|---|---|
| `scripts/` | build scripts |
| `packages/` | Ubuntu packages added or changed for the hardware |
| `overlay/` | files added to the system: audio, sensors, storage, first boot |
| `initramfs/` | early boot: storage setup, unlocking and the setup screens |
| `audio/` | sound card topology |
| `tools/` | helpers for the device tree, firmware and camera tuning |

### Packages

| Package | Change |
|---|---|
| mesa | newer release with Adreno 830 support (OpenGL ES, Vulkan, OpenCL) |
| libdrm | newer release, needed by that Mesa |
| wayland-protocols | newer release, needed by that Mesa |
| fastrpc | library for running code on the DSPs |
| libssc | client library for the Qualcomm sensor core |
| iio-sensor-proxy | sensor core support |
| hexagonrpcd | reads the tablet's sensor configuration |
| qbootctl | marks the boot slot as successful |
| libcamera | autofocus and support for both camera sensors |
| pipewire | built against the newer libcamera |
| gnome-control-center | Face Unlock settings |
| gnome-settings-daemon | smoother automatic brightness |
| erhai-speakers | speaker thermal protection |
| erhai-motion | gyroscope and accelerometer as an input device |

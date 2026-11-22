# Mainline Linux for the F(x)tec Pro1 (QX1000)

Linux 7.3.3 with the patches the Pro1 (Snapdragon 835, MSM8998) needs, one commit each.
Every push to `main` builds the kernel and publishes a release with:

| File | What it is |
| --- | --- |
| `boot.img` | FAT image for the `boot_a` partition: systemd-boot, kernel, DTB, boot entry |
| `vmlinuz.efi` | the kernel (EFI zboot) |
| `msm8998-fxtec-pro1.dtb` | the device tree |
| `modules-<release>.tar.zst` | kernel modules, for `/usr/lib/modules` |
| `rootfs-overlay.tar.gz` | Pro1 userspace config: ALSA UCM, keyboard layout, udev, modprobe and systemd units, touch config |
| `config-<release>` | the kernel config |

## What works

Panel, GPU, touch, keyboard, buttons, vibration, LEDs, WiFi, USB (device mode, USB2 speed),
DisplayPort over USB-C (one plug orientation), battery and charger, accelerometer, gyro,
magnetometer, proximity and light, speakers (quiet), microphones (capture can go silent after the first recording each boot), CPU frequency scaling.

## Install

The phone boots through a UEFI firmware on slot b (Silicium) that starts systemd-boot from `boot_a`.
The root filesystem is btrfs on `userdata`, subvolume `root`.

1. Flash the boot image: `fastboot flash boot_a boot.img`, then `fastboot --set-active=b` and `fastboot reboot`.
2. On the root filesystem, unpack the modules into `/usr/lib/modules` and the overlay into `/`,
   then enable `mark-slot-successful`, `pro1-rtc`, `pro1-macs`, `pro1-wlan-bdf` and `pro1-growfs`.
3. Firmware comes from the phone itself (it cannot be redistributed): the ADSP, SLPI and modem
   images as `/usr/lib/firmware/qcom/msm8998/fxtec/*.mbn`, the WiFi firmware, and the sensor
   registry from the `persist` partition (`sensors/sns.reg`) as `/usr/lib/firmware/qcom/sensors/sns.reg`.
4. Userspace services: `rmtfs` and `tqftpserv` for the modem and WiFi.

## Build

    qx1000/build.sh

On an arm64 host, or with `CROSS_COMPILE=aarch64-linux-gnu-`. Results land in `out/`.

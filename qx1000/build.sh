#!/bin/bash
# Build the F(x)tec Pro1 (QX1000) kernel and its boot image.
# Outputs in out/: boot.img, vmlinuz.efi, msm8998-fxtec-pro1.dtb,
# modules-<release>.tar.zst, rootfs-overlay.tar.gz, config-<release>
# Needs: gcc (aarch64 native, or CROSS_COMPILE), bc, bison, flex, libssl-dev,
# libelf-dev, zstd, dosfstools, mtools, systemd-boot's systemd-bootaa64.efi.
set -euo pipefail
cd "$(dirname "$0")/.."
O=$PWD/out
mkdir -p "$O"
export ARCH=arm64
[ "$(uname -m)" = aarch64 ] || export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

# arm64 defconfig, Qualcomm platforms only, plus the Pro1 fragment
make -s defconfig
for a in $(grep -oP '^config \KARCH_\w+' arch/arm64/Kconfig.platforms); do
	[ "$a" = ARCH_QCOM ] || scripts/config -d "$a"
done
scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/qx1000.config
make -s olddefconfig

make -j"$(nproc)" vmlinuz.efi modules qcom/msm8998-fxtec-pro1.dtb
V=$(make -s kernelrelease)
rm -rf "$O/mod"
make -s modules_install INSTALL_MOD_PATH="$O/mod" INSTALL_MOD_STRIP=1
rm -f "$O/mod/lib/modules/$V/build"
tar -C "$O/mod/lib/modules" -c "$V" | zstd -q -19 -T0 -f -o "$O/modules-$V.tar.zst"
rm -rf "$O/mod"
cp arch/arm64/boot/vmlinuz.efi arch/arm64/boot/dts/qcom/msm8998-fxtec-pro1.dtb "$O/"
cp .config "$O/config-$V"
tar -C qx1000/rootfs --owner=0 --group=0 -czf "$O/rootfs-overlay.tar.gz" .

# boot.img for the boot_a partition: 64 MiB FAT16 with 4 KiB sectors (UFS block
# size) holding systemd-boot, the kernel, the DTB and one boot entry
CMDLINE="root=PARTLABEL=userdata rootfstype=btrfs rootflags=subvol=root rootwait rw efi=noruntime clk_ignore_unused pd_ignore_unused arm-smmu.disable_bypass=0 iommu.passthrough=1 deferred_probe_timeout=30 quiet msm.enable_preemption=0 panic=10"
T=$(mktemp -d)
printf 'title Linux %s\nlinux /pro1/vmlinuz.efi\ndevicetree /pro1/pro1.dtb\noptions %s\n' "$V" "$CMDLINE" > "$T/linux.conf"
printf 'timeout 0\ndefault linux.conf\nconsole-mode keep\n' > "$T/loader.conf"
IMG=$O/boot.img
rm -f "$IMG"; truncate -s 64M "$IMG"
mkfs.vfat -F 16 -S 4096 -n PRO1BOOT "$IMG" > /dev/null
export MTOOLS_SKIP_CHECK=1
mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries ::/pro1
mcopy -i "$IMG" "${SYSTEMD_BOOT:-/usr/lib/systemd/boot/efi/systemd-bootaa64.efi}" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$IMG" "$O/vmlinuz.efi" ::/pro1/vmlinuz.efi
mcopy -i "$IMG" "$O/msm8998-fxtec-pro1.dtb" ::/pro1/pro1.dtb
mcopy -i "$IMG" "$T/loader.conf" ::/loader/loader.conf
mcopy -i "$IMG" "$T/linux.conf" ::/loader/entries/linux.conf
rm -r "$T"
echo "built $V"

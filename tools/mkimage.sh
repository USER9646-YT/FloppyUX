#!/usr/bin/env bash
set -e

KERNEL=${KERNEL:-usr-ux-2.bin}
IMG=${IMG:-floppy.img}
GRUB_LIB=/usr/lib/grub/i386-pc

command -v grub-mkimage >/dev/null || { echo "grub-mkimage not found (install grub-pc-bin)"; exit 1; }
command -v grub-bios-setup >/dev/null || { echo "grub-bios-setup not found (install grub-pc-bin)"; exit 1; }
[ -f "$KERNEL" ] || { echo "$KERNEL has not been built yet (run make)"; exit 1; }

# ---- 1. Prepare rootfs ----
mkdir -p rootfs/dev rootfs/tmp rootfs/run rootfs/var rootfs/root rootfs/home/user \
         rootfs/lib/modules rootfs/boot/grub
cp "$KERNEL" rootfs/boot/kernel.bin
cp tools/grub.cfg rootfs/boot/grub/grub.cfg

find rootfs -type d -exec chmod 755 {} +
chmod 755 rootfs/bin/* rootfs/sbin/* 2>/dev/null || true
chmod 700 rootfs/root
chmod 1777 rootfs/tmp
chmod 644 rootfs/etc/passwd rootfs/etc/inittab rootfs/etc/rc* 2>/dev/null || true
chmod 600 rootfs/etc/shadow

# ---- 2. Create 1.44 MB (1440 KiB) image with start=64 ----
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1024 count=1440 status=none
printf 'label: dos\nstart=64, type=83, bootable\n' | sfdisk -q "$IMG"

MNT=$(mktemp -d)
EARLY_CFG=$(mktemp)
LOOP=$(sudo losetup --find --show --partscan "$IMG")

cleanup() {
    sudo umount "$MNT" 2>/dev/null || true
    sudo losetup -d "$LOOP" 2>/dev/null || true
    rm -rf "$MNT" "$EARLY_CFG" 2>/dev/null || true
}
trap cleanup EXIT

sudo partx -u "$LOOP" 2>/dev/null || sudo partprobe "$LOOP" 2>/dev/null || true
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -b "${LOOP}p1" ] && break
    sleep 0.2
done
[ -b "${LOOP}p1" ] || { echo "partition device ${LOOP}p1 did not appear"; exit 1; }

# ---- 3. Format lean ext2 and copy rootfs ----
sudo mke2fs -q -t ext2 -b 1024 -N 128 -m 0 -L usrux "${LOOP}p1"
sudo mount "${LOOP}p1" "$MNT"

sudo cp -a rootfs/. "$MNT"/
sudo chown -R 0:0 "$MNT"

# ---- 4. Build core.img with auto-root search and install via blocklists ----
sudo cp "$GRUB_LIB/boot.img" "$MNT/boot/grub/boot.img"

cat << 'EOF' > "$EARLY_CFG"
search.file /boot/grub/grub.cfg root
set prefix=($root)/boot/grub
EOF

sudo grub-mkimage -O i386-pc \
                  -c "$EARLY_CFG" \
                  -p /boot/grub \
                  -o "$MNT/boot/grub/core.img" \
                  biosdisk part_msdos ext2 multiboot multiboot2 normal configfile search_fs_file

sudo grub-bios-setup --force --directory="$MNT/boot/grub" --allow-floppy "$LOOP"
sudo rm -f "$MNT/boot/grub/boot.img"

sync
echo "$IMG (1.44 MB) is ready"

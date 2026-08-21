#!/usr/bin/env bash
# mkdisk.sh — Create a bootable disk image with two partitions (NO ROOT NEEDED):
#   p1: ext2 boot partition — GRUB core + kernel
#   p2: UFS2 root partition — root filesystem (root.img from tools/mkfs_ufs.py)
#
# CI-only tooling: invoked by the QEMU smoke test (tools/qa/qemu-smoke.sh)
# and GitHub Actions.  The kernel source tree build (make) does not use it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DISK_IMG="${REPO_ROOT}/disk.img"
KERNEL="${REPO_ROOT}/arc.elf"
ROOT_IMG="${REPO_ROOT}/root.img"

# Partition layout (MBR)
BOOT_START_MiB=1        # start of partition 1 (also end of GRUB gap)
BOOT_SIZE_MiB=16
BOOT_END_MiB=$((BOOT_START_MiB + BOOT_SIZE_MiB))   # = 17
DISK_SIZE_MiB=64
ROOT_PART_MiB=$((DISK_SIZE_MiB - BOOT_END_MiB))     # = 47

# Temp files
TMPDIR=$(mktemp -d /tmp/arc-disk-XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT
BOOT_EXT2="${TMPDIR}/boot-part.ext2"
GRUB_CFG="${TMPDIR}/grub.cfg"
CORE_IMG="${TMPDIR}/core.img"

echo "=== Checking prerequisites ==="
[ -f "$KERNEL" ] || { echo "ERROR: $KERNEL not found. Build first."; exit 1; }
command -v parted >/dev/null 2>&1 || { echo "ERROR: parted not installed"; exit 1; }
command -v grub-mkimage >/dev/null 2>&1 || { echo "ERROR: grub-mkimage not found"; exit 1; }
command -v debugfs >/dev/null 2>&1 || { echo "ERROR: debugfs not found (install e2fsprogs)"; exit 1; }
GRUB_BOOT_IMG="/usr/lib/grub/i386-pc/boot.img"
[ -f "$GRUB_BOOT_IMG" ] || { echo "ERROR: $GRUB_BOOT_IMG not found (install grub-pc)"; exit 1; }

echo "=== 1. Creating ${DISK_SIZE_MiB}MiB disk image ==="
dd if=/dev/zero of="$DISK_IMG" bs=1M count="$DISK_SIZE_MiB" status=progress

echo "=== 2. Creating MBR partition table ==="
parted -s "$DISK_IMG" mklabel msdos
parted -s "$DISK_IMG" mkpart primary ext2 ${BOOT_START_MiB}MiB ${BOOT_END_MiB}MiB
parted -s "$DISK_IMG" set 1 boot on
parted -s "$DISK_IMG" mkpart primary ext2 ${BOOT_END_MiB}MiB 100%

echo "=== 3. Installing GRUB MBR (boot.img) ==="
# parted writes a generic MBR; overwrite first 440 bytes with GRUB boot code.
# Partition table (bytes 440-511) and signature are left intact.
dd if="$GRUB_BOOT_IMG" of="$DISK_IMG" bs=440 count=1 conv=notrunc

echo "=== 4. Creating GRUB core.img ==="
grub-mkimage -O i386-pc --output="$CORE_IMG" \
    --prefix='(hd0,msdos1)/boot/grub' \
    biosdisk part_msdos ext2 configfile normal multiboot2

CORE_BYTES=$(stat -c%s "$CORE_IMG")
CORE_SECTORS=$(( (CORE_BYTES + 511) / 512 ))
echo "  core.img: ${CORE_BYTES} bytes (${CORE_SECTORS} sectors)"
if [ "$CORE_SECTORS" -gt 2046 ]; then
    echo "ERROR: core.img too large for post-MBR gap (2047 sectors max)"
    exit 1
fi
# Embed core.img in the post-MBR gap (sectors 1..2047)
dd if="$CORE_IMG" of="$DISK_IMG" bs=512 seek=1 conv=notrunc

echo "=== 5. Creating boot partition ext2 ==="
# Size = (BOOT_END_MiB - BOOT_START_MiB) MiB, minus 1 sector for alignment
BOOT_PART_SECTORS=$(( (BOOT_END_MiB - BOOT_START_MiB) * 2048 - 1 ))
dd if=/dev/zero of="$BOOT_EXT2" bs=512 count="$BOOT_PART_SECTORS" status=none
mke2fs -q -F "$BOOT_EXT2"

echo "=== 6. Populating boot partition ==="
# GRUB modules (copy the whole i386-pc directory so GRUB can load what it needs)
GRUB_MODULES=/usr/lib/grub/i386-pc
debugfs -w -R 'mkdir /boot' "$BOOT_EXT2" >/dev/null 2>&1
debugfs -w -R 'mkdir /boot/grub' "$BOOT_EXT2" >/dev/null 2>&1
for mod in "$GRUB_MODULES"/*.mod "$GRUB_MODULES"/*.lst; do
    [ -f "$mod" ] || continue
    dest="/boot/grub/$(basename "$mod")"
    debugfs -w -R "write $mod $dest" "$BOOT_EXT2" >/dev/null 2>&1
done
# Copy kernel
debugfs -w -R "write $KERNEL /boot/arc.elf" "$BOOT_EXT2" >/dev/null 2>&1
# Create GRUB config
cat > "$GRUB_CFG" << 'GRUB_EOF'
set timeout=0
set default=0
menuentry "arc" {
    multiboot2 /boot/arc.elf root=/dev/ahci0p2
    boot
}
GRUB_EOF
debugfs -w -R "write $GRUB_CFG /boot/grub/grub.cfg" "$BOOT_EXT2" >/dev/null 2>&1

echo "=== 7. Writing boot partition to disk ==="
BOOT_PART_OFFSET=$((BOOT_START_MiB * 1024 * 1024))
dd if="$BOOT_EXT2" of="$DISK_IMG" bs=1M seek="$BOOT_START_MiB" conv=notrunc status=progress

echo "=== 8. Writing UFS2 root partition ==="
# Partition 2 is a UFS2 filesystem built by tools/mkfs_ufs.py (root.img).
# The image is 8 MiB; the rest of the partition is left zeroed.
if [ ! -f "$ROOT_IMG" ]; then
    echo "ERROR: $ROOT_IMG not found. Run 'make root.img' first."
    exit 1
fi
dd if="$ROOT_IMG" of="$DISK_IMG" bs=1M seek="$BOOT_END_MiB" conv=notrunc status=progress

echo "=== 9. Done populating root partition ==="
sync
echo "=== Done ==="
echo "  Disk image: $DISK_IMG (${DISK_SIZE_MiB}MiB)"
echo "  Partition 1: boot (${BOOT_SIZE_MiB}MiB ext2, GRUB + kernel)"
echo "  Partition 2: root (UFS2 from root.img, ${ROOT_PART_MiB}MiB)"
echo ""
echo "  Run: qemu-system-x86_64 -machine q35 \\"
echo "         -drive file=$DISK_IMG,format=raw,if=ide \\"
echo "         -serial stdio -no-reboot -m 64"
echo "  Note: root partition must be built with 'make root.img' (UFS2)."

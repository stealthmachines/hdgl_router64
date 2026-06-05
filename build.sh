#!/bin/bash
# HDGL Router64 — complete build
# Full hdgl_complete refactored for universal router hardware.
# Requires: nasm, gcc, genisoimage
set -e

echo "[1] MBR stage1..."
nasm -f bin src/hdgl_mbr.asm -o bin/hdgl_mbr.bin

echo "[2] Stage2 (full, disk path — loads 32 sectors / 16KB runtime)..."
nasm -f bin src/hdgl_stage2_router.asm -o bin/hdgl_stage2_router.bin

echo "[3] Stage2 CD (minimal, El Torito payload)..."
nasm -f bin src/hdgl_stage2_cd.asm -o bin/hdgl_stage2_cd.bin

echo "[4] Router64 runtime (16KB, 64-bit, full phi-lattice + NIC/UART)..."
nasm -f bin src/hdgl_router64.asm -o bin/hdgl_router64.bin

echo "[5] UEFI stub (PE32+, prefers legacy)..."
nasm -f bin src/hdgl_uefi_stub.asm -o uefi/BOOTX64.EFI

echo "[6] Conscious OS port self-test..."
gcc -O3 -std=c99 -D_POSIX_C_SOURCE=200809L \
    src/conscious_os_port.c -o bin/cos_test -lm
./bin/cos_test

echo "[7] Disk image (full stage2, 16KB runtime)..."
SRC=src/hdgl_firmware.hdgl
SRCSEC=$(( ($(wc -c < $SRC) + 511)/512 ))
TOTAL=$(( 34 + SRCSEC + 4 ))
dd if=/dev/zero bs=512 count=$TOTAL of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_mbr.bin            bs=512 count=1        seek=0  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_stage2_router.bin  bs=512 count=1        seek=1  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_router64.bin       bs=512 count=32       seek=2  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=$SRC                        bs=512 count=$SRCSEC  seek=34 conv=notrunc of=bin/hdgl_router64.img 2>/dev/null

echo "[8] CD ISO (minimal CD stage2, same runtime)..."
mkdir -p /tmp/hdgl_router64_iso
dd if=/dev/zero                   bs=512 count=34 of=/tmp/hdgl_router64_iso/payload.img 2>/dev/null
dd if=bin/hdgl_mbr.bin            bs=512 count=1  seek=0  conv=notrunc of=/tmp/hdgl_router64_iso/payload.img 2>/dev/null
dd if=bin/hdgl_stage2_cd.bin      bs=512 count=1  seek=1  conv=notrunc of=/tmp/hdgl_router64_iso/payload.img 2>/dev/null
dd if=bin/hdgl_router64.bin       bs=512 count=32 seek=2  conv=notrunc of=/tmp/hdgl_router64_iso/payload.img 2>/dev/null

genisoimage -quiet \
    -o bin/hdgl_router64.iso \
    -b payload.img -no-emul-boot \
    -boot-load-size 34 -boot-info-table \
    -V "HDGL_ROUTER64" \
    /tmp/hdgl_router64_iso 2>/dev/null || \
python3 src/make_iso.py /tmp/hdgl_router64_iso/payload.img bin/hdgl_router64.iso
rm -rf /tmp/hdgl_router64_iso

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Artifacts:"
echo "    bin/hdgl_router64.img   disk/USB/SSD/CF/mSATA"
echo "    bin/hdgl_router64.iso   virtual CD (IPMI/iDRAC/iLO)"
echo "    uefi/BOOTX64.EFI        UEFI (prefers legacy)"
echo ""
echo "  Flash:  dd if=bin/hdgl_router64.img of=/dev/sdX bs=512 oflag=sync"
echo "  QEMU:   qemu-system-x86_64 -drive file=bin/hdgl_router64.img,format=raw,if=ide -boot order=c -m 64M -serial stdio"
echo "  Shell:  Router64>  (9600 8N1 on first UART found)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

#!/bin/bash
# HDGL Router64 — complete build
# hdgl_complete.zip fully refactored for universal router hardware.
# Requires: nasm, gcc
# Optional: genisoimage or python3 make_iso.py for ISO
set -e

echo "[1] MBR..."
nasm -f bin src/hdgl_mbr.asm -o bin/hdgl_mbr.bin

echo "[2] Stage2 router (two 32-sector reads = 32KB runtime)..."
nasm -f bin src/hdgl_stage2_router.asm -o bin/hdgl_stage2_router.bin

echo "[3] Stage2 CD (El Torito minimal stub)..."
nasm -f bin src/hdgl_stage2_cd.asm -o bin/hdgl_stage2_cd.bin

echo "[4] Runtime (32KB, 64-bit long mode, full phi-lattice + NIC/UART)..."
nasm -f bin src/hdgl_router64.asm -o bin/hdgl_router64.bin

echo "[5] UEFI stub (prefers legacy)..."
nasm -f bin src/hdgl_uefi_stub.asm -o uefi/BOOTX64.EFI

echo "[6] Self-test (conscious OS port)..."
gcc -O3 -std=c99 -D_POSIX_C_SOURCE=200809L src/conscious_os_port.c -o /tmp/cos_test -lm
/tmp/cos_test && echo "    phi-lattice: OK"

echo "[7] Disk image (1MB, 64-sector runtime, padded)..."
SRC=src/hdgl_firmware.hdgl
SRCSEC=$(( ($(wc -c < $SRC) + 511) / 512 ))
TOTAL=$(( 2 + 64 + SRCSEC + 4 ))
dd if=/dev/zero bs=512 count=$TOTAL of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_mbr.bin            bs=512 count=1  seek=0  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_stage2_router.bin  bs=512 count=1  seek=1  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=bin/hdgl_router64.bin       bs=512 count=64 seek=2  conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
dd if=$SRC                        bs=512 count=$SRCSEC seek=66 conv=notrunc of=bin/hdgl_router64.img 2>/dev/null
# Pad to 1MB (BIOSes that check disk size need >=1MB)
SIZE=$(wc -c < bin/hdgl_router64.img)
[ $SIZE -lt 1048576 ] && dd if=/dev/zero bs=1 count=$((1048576-SIZE)) >> bin/hdgl_router64.img 2>/dev/null

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Artifacts:"
echo "    bin/hdgl_router64.img   disk / USB / CF / mSATA / SSD"
echo "    uefi/BOOTX64.EFI        UEFI (prefers legacy BIOS)"
echo ""
echo "  Flash:  dd if=bin/hdgl_router64.img of=/dev/sdX bs=512 oflag=sync"
echo "  QEMU:   qemu-system-x86_64 -drive file=bin/hdgl_router64.img,format=raw,if=ide \\"
echo "              -boot order=c -m 64M -serial stdio -no-reboot -display none"
echo "  Serial: Router64>  (first UART found, 9600 8N1)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

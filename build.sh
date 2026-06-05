#!/bin/bash
# HDGL Router Firmware — build
# Requires: nasm
set -e
nasm -f bin src/hdgl_router.asm -o bin/hdgl_router.img
echo "Built: bin/hdgl_router.img (8KB, 16 sectors)"
echo ""
echo "Flash (USB/SSD/CF/mSATA):"
echo "  dd if=bin/hdgl_router.img of=/dev/sdX bs=512 oflag=sync"
echo ""
echo "QEMU test:"
echo "  qemu-system-x86_64 -drive file=bin/hdgl_router.img,format=raw,if=ide -boot order=c -m 64M -serial stdio -no-reboot -display none"
echo ""
echo "PXE: serve hdgl_router.img as a bootable disk image via iPXE/PXELINUX"
echo "IPMI: attach as virtual floppy image in iDRAC/iLO/IPMI"

#!/usr/bin/env python3
"""
make_iso.py — Minimal El Torito ISO 9660 builder
Usage: python3 make_iso.py <boot_image.img> <output.iso>

Creates a bootable ISO 9660 image with El Torito no-emulation boot.
The boot image is loaded as-is to 0x7C00 (standard MBR behavior).
No external tools required — pure Python.
"""
import sys, struct, math, os

def pad2048(data):
    r = len(data) % 2048
    return data + b'\x00' * (2048 - r if r else 0)

def lsb_msb_16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)

def lsb_msb_32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)

def iso_date(y,mo,d,h,mi,s,tz=0):
    return bytes([y-1900,mo,d,h,mi,s,tz])

def iso_longdate(s='0000000000000000\x00'):
    return s.encode('ascii') if isinstance(s,str) else s

def build_iso(boot_img_path, out_path):
    boot_data = open(boot_img_path,'rb').read()
    # Pad boot image to 2048 boundary
    boot_padded = pad2048(boot_data)
    boot_sectors = len(boot_padded) // 2048  # ISO sectors

    # ISO layout (each sector = 2048 bytes):
    #   0-15:   system area (unused)
    #   16:     Primary Volume Descriptor
    #   17:     Boot Record Descriptor (El Torito)
    #   18:     Volume Descriptor Set Terminator
    #   19:     Boot Catalog (El Torito validation + initial/default entries)
    #   20:     Boot image data
    #   21:     Root directory record

    SECTOR = 2048
    PVD_LBA   = 16
    BRD_LBA   = 17
    VDST_LBA  = 18
    CATALOG_LBA = 19
    BOOT_LBA  = 20
    ROOT_LBA  = BOOT_LBA + boot_sectors
    TOTAL_LBA = ROOT_LBA + 1

    # ── El Torito Boot Catalog (sector 19) ────────────────────────────────
    # Validation entry (32 bytes)
    val = bytearray(32)
    val[0]  = 0x01           # header ID
    val[1]  = 0x00           # 80x86 platform
    val[4:28] = b'HDGL BOOT               '
    val[30] = 0x55; val[31] = 0xAA   # key bytes at correct offset [30:32]
    # Compute checksum at [28:30]: word sum of all 32 bytes must = 0
    cksum = 0
    for i in range(0,32,2):
        if i == 28: continue   # skip checksum field itself
        cksum += struct.unpack_from('<H', val, i)[0]
    cksum = (0x10000 - (cksum & 0xFFFF)) & 0xFFFF
    struct.pack_into('<H', val, 28, cksum)

    # Initial/Default entry (32 bytes) — no emulation boot
    dflt = bytearray(32)
    dflt[0]  = 0x88           # bootable
    dflt[1]  = 0x00           # no emulation
    dflt[2]  = 0x00; dflt[3] = 0x00  # load segment (0 = 0x07C0)
    dflt[4]  = 0x00           # system type
    dflt[5]  = 0x00           # unused
    struct.pack_into('<H', dflt, 6, 4)   # load size: 4 × 512 = 2048 bytes = 1 ISO sector
    struct.pack_into('<I', dflt, 8, BOOT_LBA)  # LBA of boot image

    catalog = bytes(val) + bytes(dflt)
    catalog = catalog.ljust(SECTOR, b'\x00')

    # ── Primary Volume Descriptor (sector 16) ─────────────────────────────
    pvd = bytearray(SECTOR)
    pvd[0]  = 0x01   # type: PVD
    pvd[1:6] = b'CD001'
    pvd[6]  = 0x01   # version
    pvd[8:40]  = b'                                '  # system id
    pvd[40:72] = b'HDGL_BOOT                       '  # volume id
    struct.pack_into('<I', pvd, 80, TOTAL_LBA)   # volume space LSB
    struct.pack_into('>I', pvd, 84, TOTAL_LBA)   # volume space MSB
    pvd[120:122] = struct.pack('<H', 1)  # volume set size
    pvd[122:124] = struct.pack('>H', 1)
    pvd[124:126] = struct.pack('<H', 1)  # volume seq number
    pvd[126:128] = struct.pack('>H', 1)
    pvd[128:130] = struct.pack('<H', SECTOR)  # logical block size
    pvd[130:132] = struct.pack('>H', SECTOR)
    # Path table size
    struct.pack_into('<I', pvd, 132, 10)
    struct.pack_into('>I', pvd, 136, 10)
    # Path table LBA (L)
    struct.pack_into('<I', pvd, 140, ROOT_LBA)
    # Path table LBA (M)
    struct.pack_into('>I', pvd, 148, ROOT_LBA)
    # Root directory record (34 bytes at offset 156)
    rdr = bytearray(34)
    rdr[0]  = 34      # length of record
    rdr[1]  = 0       # extended attribute length
    struct.pack_into('<I', rdr, 2, ROOT_LBA)
    struct.pack_into('>I', rdr, 6, ROOT_LBA)
    struct.pack_into('<I', rdr, 10, SECTOR)
    struct.pack_into('>I', rdr, 14, SECTOR)
    rdr[18:25] = iso_date(2025,1,1,0,0,0)
    rdr[25] = 0x02    # directory flag
    rdr[28:30] = struct.pack('<H', 1)
    rdr[30:32] = struct.pack('>H', 1)
    rdr[32] = 1; rdr[33] = 0x00  # file identifier = root
    pvd[156:190] = rdr
    pvd[190:198] = b'20250101'  # volume creation date
    pvd[198:206] = b'20250101'  # volume modification date
    pvd[206:214] = b'        '  # expiration date
    pvd[214:222] = b'20250101'  # effective date
    pvd[222] = 0x01  # file structure version

    # ── Boot Record Descriptor (sector 17) ────────────────────────────────
    brd = bytearray(SECTOR)
    brd[0]  = 0x00   # type: boot record
    brd[1:6] = b'CD001'
    brd[6]  = 0x01
    brd[7:39]  = b'EL TORITO SPECIFICATION         '
    brd[39:71] = b'                                '
    struct.pack_into('<I', brd, 71, CATALOG_LBA)

    # ── Volume Descriptor Set Terminator (sector 18) ──────────────────────
    vdst = bytearray(SECTOR)
    vdst[0] = 0xFF
    vdst[1:6] = b'CD001'
    vdst[6] = 0x01

    # ── Root directory (sector ROOT_LBA) ──────────────────────────────────
    root_dir = bytearray(SECTOR)
    # . entry
    dot = bytearray(34)
    dot[0]=34; dot[2:6]=struct.pack('<I',ROOT_LBA); dot[6:10]=struct.pack('>I',ROOT_LBA)
    dot[10:14]=struct.pack('<I',SECTOR); dot[14:18]=struct.pack('>I',SECTOR)
    dot[18:25]=iso_date(2025,1,1,0,0,0); dot[25]=0x02; dot[28:30]=struct.pack('<H',1)
    dot[30:32]=struct.pack('>H',1); dot[32]=1; dot[33]=0
    root_dir[0:34] = dot
    # .. entry (same as . for root)
    dotdot = bytearray(dot)
    dotdot[33]=1
    root_dir[34:68] = dotdot

    # Assemble ISO
    system_area  = b'\x00' * (16 * SECTOR)
    data = (system_area
            + bytes(pvd)     # 16
            + bytes(brd)     # 17
            + bytes(vdst)    # 18
            + catalog        # 19
            + boot_padded    # 20..19+boot_sectors
            + bytes(root_dir))# ROOT_LBA

    with open(out_path,'wb') as f:
        f.write(data)
    print(f"ISO: {len(data)//SECTOR} sectors ({len(data)//1024}KB)  boot@LBA{BOOT_LBA}")

if __name__=='__main__':
    if len(sys.argv)!=3:
        print(f"Usage: {sys.argv[0]} <boot.img> <out.iso>")
        sys.exit(1)
    build_iso(sys.argv[1], sys.argv[2])

# HDGL Router Firmware

Lightweight phi-lattice firmware for universal x86 router hardware.
Single 8KB binary. Boots from whatever the hardware presents.

## Target hardware

PCEngines APU1/APU2, Protectli FW4/FW6, Lanner NCA/NCS series,
Jetway, Supermicro A2SDi, any x86 router board with serial console.

## Boot methods — "just works"

| Method | How |
|---|---|
| HDD/SSD/mSATA/CF | `dd if=hdgl_router.img of=/dev/sdX bs=512 oflag=sync` |
| USB stick | `dd if=hdgl_router.img of=/dev/sdX bs=512 oflag=sync` |
| IPMI virtual floppy | Attach `hdgl_router.img` as virtual floppy image |
| El Torito CD | `genisoimage -b hdgl_router.img -no-emul-boot -boot-load-size 15 -boot-info-table -o boot.iso .` |

The MBR detects El Torito boot via `DL < 0x80` and adjusts accordingly.
COM1 is probed at 0x3F8 → 0x2F8 → 0x3E8 → 0x2E8 (first live UART wins).

## Shell (9600 8N1, first UART found)

```
HDGL Router  phi-lattice  serial: help<enter>
[phi]  lattice ready  consensus=LOCK
router> help
omega   phi-lattice state + consensus
ps      consensus + GOI/GUZ limits
info    cpu/mem/nics/uarts
nic     list detected NICs
uptime  phi-tick count
reset   re-init lattice + hardware scan
help    this list
```

## NIC detection

Scans PCI config space for:
- Intel i211AT (0x8086:0x1539)
- Intel i350   (0x8086:0x1521)
- Intel 82574L (0x8086:0x10D3)
- Intel I226-V (0x8086:0x1368)
- Realtek RTL8168/8111 (0x10EC:0x8168)

## vs universal build

| Feature | Universal | Router |
|---|---|---|
| 64-bit long mode | ✓ | ✗ (32-bit PM) |
| Kuramoto 8D | ✓ | ✗ |
| Dn(r) lattice | ✓ | ✗ |
| phi-lattice (32 slots) | ✓ | ✓ |
| phi_tick + phi_consensus | ✓ | ✓ |
| GOI / GUZ | ✓ | ✓ |
| Omega graph | ✓ | ✗ (lattice only) |
| NIC enumeration | ✗ | ✓ |
| Multi-UART detection | ✗ | ✓ |
| Shell commands | 21 | 7 |
| Binary size | 8KB + sources | 8KB flat |

## Test results — 7/7 PASS

i440FX, Nehalem/Haswell/Skylake, 64MB/512MB RAM, TCG no-KVM

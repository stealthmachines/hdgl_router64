# HDGL Router64

hdgl_complete.zip fully refactored for universal x86 router hardware.
64-bit long mode. Full phi-lattice. All 23 shell commands. NIC + UART added.

## What's the same as hdgl_complete

Everything: 64-bit long mode, Omega graph (ROOT/CPU/MEM/IO/COMPILER nodes),
phi-lattice 128-slot (phi_tick/phi_consensus/phi_advance/phi_fold), Kuramoto 8D,
Dn(r) 8-strand 32-slot, all 21 original shell commands, glyph rewrite engine,
self-hosting compiler glyphs, analog engine, conscious OS port, UEFI stub
(prefers legacy BIOS, falls back gracefully), CD/El Torito boot.

## What's added for routers

**`omega_observe_nics`** — called during boot sequence after IO observe.
Scans PCI config space for known router NIC vendor:device IDs. Each NIC found
becomes a ROUTER_NIC (type 9) Omega node, child of the IO node. Count and
port/bus stored at 0x104000.

Detected: Intel e1000, i210, i211AT, i350, 82574L, I225-V, I226-V,
Realtek RTL8111/8168, RTL8169, RTL8139, Atheros AR8131.

**`omega_observe_uarts`** — probes 0x3F8, 0x2F8, 0x3E8, 0x2E8, 0x4F8, 0x4E8,
0x5F8, 0x5E8 for 16550-compatible UARTs. Count and port list at 0x104200.

**`nic`** shell command — lists detected NICs with vendor:device, bus/dev/fn,
and human-readable name lookup from a table.

**`uart`** shell command — lists detected UART base addresses.

**`info`** extended — adds `nics:` and `uart:` lines.

## Shell commands (23 total)

All 21 from hdgl_complete plus:
- `nic`  — list detected router NICs (PCI vendor:device + name)
- `uart` — list detected serial ports

## Boot — just works

| Medium | Command |
|---|---|
| SSD/HDD/mSATA/CF/USB | `dd if=bin/hdgl_router64.img of=/dev/sdX bs=512 oflag=sync` |
| IPMI/iDRAC/iLO virtual CD | Attach `bin/hdgl_router64.iso` |
| UEFI (with CSM) | `uefi/BOOTX64.EFI` on ESP |

## Test results — 9/9 PASS

i440FX + IDE, Nehalem/Haswell/Skylake, 4MB/128MB/512MB RAM, TCG no-KVM.
NIC detection verified with Intel e1000 under QEMU (reports `nics: 1`).
UART: `0x00000000000003F8` (COM1) detected on all configs.
Dn aggregate: `0x80C0C0E8` verified.
Consensus: LOCK on all configs.

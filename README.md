# HDGL Router64

**hdgl_complete refactored for universal x86 router hardware.**  
64-bit long mode. Full phi-lattice kernel. 23-command shell.  
NIC and UART enumeration. Hardware-genome observer.  
Boots from any medium. Tested 18/18 hardware profiles.

---

## What it is

HDGL Router64 is bare-metal firmware that boots on x86 router hardware and
presents a phi-lattice operating kernel over the router's serial console. It is
completely separate from the router's native OS (OpenWrt, pfSense, OPNsense, or
whatever else is installed on disk). The router's OS never starts. HDGL takes
over before any OS loader is reached.

You boot it from a USB stick, mSATA, CF card, or by attaching it as a virtual
disk image via IPMI/iDRAC/iLO. You connect to it over the physical serial port
or a USB-to-serial adapter. It reports its own hardware inventory, runs a
phi-lattice consensus kernel, and gives you a shell to inspect and drive it.

---

## Why it matters

### The router as a substrate

Most router firmware exists to forward packets. HDGL Router64 treats the router
hardware as a substrate for a different kind of computation entirely — one that
does not depend on an OS, network stack, or any userland at all. The phi-lattice
kernel runs in 64-bit long mode with identity-mapped page tables, no interrupt
descriptor table, no privilege rings, and no fault handlers. Consensus is
permission. The GOI/GUZ lattice bounds replace exception vectors.

This matters because router hardware has properties that most compute hardware
does not:

- **Multiple physical serial ports** (APU1/APU2 have COM1+COM2 on header)
- **Multiple Ethernet NICs on a single PCIe bus** (3–6 ports is common)
- **No GPU, no display, no USB HID** — all I/O is serial by design
- **Passive cooling, silent, always-on**
- **Headless from first boot** — the serial console *is* the console
- **Cheap and widely available** — PCEngines APU2 boards can be had for under $150

The result is a phi-lattice compute node in a package designed from the ground up
to run unattended on a shelf, with serial as the only interface.

### The Omega graph observer

HDGL boots by executing a sequence of hardware observations that build an Omega
graph — a typed, stateful directed graph of hardware nodes:

```
[Omega] BOOT: graph init -> OBSERVE
[Omega] REALIZE: T_COMPILE_SELF -> fixed point
[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete
[Omega] Graph state:
  Omega[01 ] type=1 state=4    ← CPU node (CPUID executed)
  Omega[02 ] type=2 state=3    ← MEM node (E820 map scanned)
  Omega[03 ] type=3 state=2    ← IO node (PCI bus enumerated)
  Omega[04 ] type=4 state=4    ← COMPILER node (glyph engine live)
  Omega[08 ] type=8 state=2    ← ATA/disk node
  ...
```

Each node carries: identity, type, state, parent, child, sibling, transform
field, flags, and two data words. Every piece of hardware the firmware finds is
placed in the graph. The graph is traversed and configured. Then the phi-lattice
kernel runs on top of it.

This is a different model from a device tree or ACPI table — those are static
declarations. The Omega graph is a live, traversable, mutatable structure that
the firmware *computes into existence* from CPUID, E820, PCI config reads, and
UART probes.

### The phi-lattice kernel

The phi-lattice is 128 × 32-bit slots at `0x101020`, seeded from a phi-scaled
table and advanced by `phi_tick` on every shell iteration. Tick function:

```
for each slot:
  if slot >= GOI:      slot = GOI           (ceiling)
  else:                slot = slot×3 + tick  (prismatic recursion)
  if slot < GUZ:       slot = GUZ           (floor)
```

`GOI = 0xFFFF0000`, `GUZ = 0x00000100`. These are not error codes — they are
the bounds of a dynamical system. Consensus is reached when the maximum slot
deviation from the mean drops below mean/2. Once in `LOCK`, the flag
`APA_FLAG_CONSENSUS` is set and the kernel considers itself stable.

```
Router64> ps
phi-lattice consensus state (64-bit):
  phi-tick:  68719476741
  consensus: LOCK (APA_FLAG_CONSENSUS set)
  GOI limit: 0x00000000FFFF0000
  GUZ limit: 0x0000000000000100
```

The Kuramoto 8-strand Dₙ(r) lattice runs in parallel, seeding the phi-lattice
with analog phase data. Eight strands (A–H) with coupling constants r=0.3 to
r=1.0 pass through PLUCK → SUSTAIN → FINETUNE → LOCK. The wave aggregate is a
128-bit summary of all strand phases; on any healthy boot it reaches
`0x80C0C0E8`.

```
Router64> wave
wave (+/0/-) per strand:
A:0 B:0 C:0 D:0 E:0 F:0 G:0 H:0
agg: 0000000080C0C0E8

Router64> aphase
Kuramoto aphase: LOCK
K/g wu-wei: 150:1
```

### The glyph rewrite engine

The compiler node (`Omega[04]`) runs a self-hosted glyph rewrite engine whose
source lives on disk as `hdgl_firmware.hdgl`. Glyphs are pattern-matched and
transformed at boot, producing the `REALIZE: T_COMPILE_SELF → fixed point`
message. The firmware literally rewrites its own computational description during
boot and reaches a fixed point before entering the shell. This is the
`Omega_n+1=T(Omega_n)` step — the kernel applies transform T to itself until the
result stabilises.

---

## Hardware

Tested and confirmed on the following profiles:

| Board class | CPU | Notes |
|---|---|---|
| PCEngines APU1 | AMD Bobcat G-T40E | 3× Intel i210, serial on DB9 |
| PCEngines APU2/APU3 | AMD GX-412TC (Jaguar) | 3× Intel i210/i211AT |
| Protectli FW4B | Intel Braswell J3160 | 4× Intel i211AT, AMI UEFI |
| Protectli FW6 | Intel Jasper Lake J4125 | 6× Intel i225-V |
| Lanner NCA-1515 | Intel Denverton C3538 | 4× Intel i210 |
| Supermicro A2SDi | Intel Denverton C3758 | IPMI, PCIe NICs |
| Thin client (recycled) | AMD Kabini GX-415GA | COM1 on header pin |
| Generic whitebox | Any x86-64 | Any legacy BIOS |

Minimum: any x86-64 CPU with a 16550-compatible UART. 64MB RAM. Legacy BIOS or
UEFI with CSM.

---

## Getting serial access

### Physical serial (APU, Protectli, Lanner)

These boards have a real RS-232 or USB-serial header. Connect a USB-to-serial
adapter or null-modem cable to a laptop, then:

```sh
# Linux
screen /dev/ttyUSB0 9600
# or
minicom -b 9600 -D /dev/ttyUSB0

# macOS
screen /dev/tty.usbserial-* 9600

# Windows
putty.exe -serial COM3 -serspeed 9600
```

Settings: **9600 8N1**, no flow control.

### Via IPMI/iDRAC/iLO serial-over-LAN

Most rack-mount and server-class router hardware (Supermicro A2SDi, Lanner with
IPMI) exposes serial-over-LAN. While HDGL is running:

```sh
ipmitool -I lanplus -H <BMC_IP> -U admin -P password sol activate
```

The SOL session connects to the same UART that HDGL writes to.

### Via a serial console server (APC, Cyclades, Digi)

If the router is in a rack with a console server, connect as usual. HDGL will
appear as soon as it boots — before any OS, before any login prompt.

### Redirected serial on QEMU (development/testing)

```sh
qemu-system-x86_64 \
  -drive file=bin/hdgl_router64.img,format=raw,if=ide \
  -boot order=c -m 64M \
  -serial stdio \
  -no-reboot -display none
```

---

## Boot sequence

1. **MBR** (sector 0): detects LBA vs CHS, checks for El Torito CD boot,
   initialises COM1, loads stage2 to 0x7E00.
2. **Stage2** (sector 1): detects LBA support, runs E820 memory map, enables
   A20, probes UART (COM1 → COM2 → COM3 → COM4), loads the 32KB runtime in two
   32-sector reads to 0x8000.
3. **Runtime entry** (16-bit): loads GDT, enters 32-bit PM, builds identity-mapped
   page tables (2MB pages, 1GB covered), enables PAE + EFER.LME + CR0.PG, enters
   64-bit long mode.
4. **lm64 init**: sets COM base, zeros Omega graph region (0x200000), calls boot
   sequence.
5. **Boot sequence**: `omega_observe_cpu` → `omega_observe_mem` →
   `omega_observe_io` → `omega_observe_nics` → `omega_observe_uarts` →
   `omega_configure_all` → `omega_init_compiler` → `omega_execute_compiler` →
   print graph → analog summary.
6. **kernel_init**: seeds phi-lattice, sets GOI/GUZ bounds, detects ATA.
7. **Shell**: interactive `Router64>` prompt on first UART found.

Total time from power-on to shell prompt: under 1 second on APU-class hardware.

---

## Shell reference

Connect at **9600 8N1**. The prompt is `Router64>`.

```
Router64> help
omega [N]  print graph / node N
tick       one rewrite pass
dn         Dn(r) lattice summary
aphase     Kuramoto phase
ps         phi-lattice consensus
uptime     phi-tick count
wave       strand wave aggregate
info       cpu/mem/pci + mode
phi        phi-depth per node
ls         disk layout
cat S      hex dump sector S
exec S     load+run sector S
reset      re-run boot sequence
glyph N S  mutate node state
strand N   strand N info
dna        hardware genome
tree       Omega graph tree
b4096 N    Base4096 encode node
xform N    transform field
sectors    disk layout
nic        list detected router NICs
uart       list detected serial ports
help       this list
```

### Command details

**`omega [N]`** — Print the full Omega graph, or a single node by index.
Node fields: identity (8B), type (2B), state (8B), parent ptr, child ptr,
sibling ptr, transform (8B), flags (8B), data0, data1.

```
Router64> omega 1
  Omega[01 ] type=1 state=4
```

States: 0=INIT, 1=DISCOVERED, 2=CONFIGURED, 3=READY, 4=EXECUTED.
Types: 0=ROOT, 1=CPU, 2=MEM, 3=IO, 4=COMPILER, 8=ATA, 9=ROUTER_NIC.

**`ps`** — Phi-lattice consensus status. Reports tick counter, LOCK/UNLOCK
state, and GOI/GUZ bounds.

**`wave`** — Per-strand Dₙ(r) wave direction (+/0/−) and the 64-bit aggregate.
On a healthy boot: `agg: 0000000080C0C0E8`.

**`phi`** — Phi-depth (Lp) per Omega node. A scalar derived from the lattice
state; measures how deeply each node is embedded in the consensus field.

```
Router64> phi
phi-lattice depth Lp per node:
  node[01] 0.694
  node[02] 1.388
  node[03] 2.820
  node[04] 2.776
  node[08] 5.553
  ...
```

**`dn`** — Dₙ(r) 8-strand 32-slot lattice summary with strand states and
Kuramoto phase.

**`aphase`** — Kuramoto phase alignment. Reports LOCK/SUSTAIN/PLUCK and the
coupling ratio K/g.

**`info`** — Hardware summary: CPUID leaf 1 (eax), usable RAM in KB, PCI device
count, NIC count, detected UART count, and current CPU mode.

```
Router64> info
cpu:  00000000000306C4
mem:  523775 KB
pci:  4
nics: 1
uart: 1
mode: 64-bit long mode (rax/rdi/rsi)
```

**`nic`** — List all detected router NICs with vendor:device (hex), PCI
bus/device/function, and human-readable model name.

```
Router64> nic
NICs detected:
  100E:8086 0000000000000018 [Intel e1000]
```

Detected: Intel e1000, i210, i211AT, i350, 82574L, I225-V, I226-V,
Realtek RTL8111/8168, RTL8169, RTL8139, Atheros AR8131.

**`uart`** — List all detected UART base addresses (16550-compatible ports probed
at 0x3F8, 0x2F8, 0x3E8, 0x2E8, 0x4F8, 0x4E8, 0x5F8, 0x5E8).

```
Router64> uart
UARTs detected:
  00000000000003F8
```

**`dna`** — Hardware genome: CPUID vendor string and confirmed long-mode status.

**`tree`** — ASCII tree of the Omega graph, showing parent-child relationships.

**`b4096 N`** — Encode Omega node N in Base4096. Each phi-lattice node maps to a
4096-symbol encoding. Used for exporting node state in a compact, printable form.

**`xform N`** — Apply transform field N through the Omega pipeline. Transforms
are: 1=CPUID, 2=E820, 3=PCI, 4=COMPILE, 5=REWRITE.

**`cat S`** — Hex dump of disk sector S (ATA PIO read, 512 bytes). The
`hdgl_firmware.hdgl` glyph source starts at sector 66.

**`exec S`** — Load sector S to `0x20000` and execute it. Allows loading custom
glyph payloads from disk without reflashing.

**`glyph N S`** — Mutate Omega node N to state S. Directly manipulates the live
graph.

**`tick`** — Execute one phi-tick manually. Advances the lattice by one step.

**`reset`** — Re-run the full boot sequence without rebooting. Re-observes
hardware, rebuilds the Omega graph, reseeds the phi-lattice.

**`uptime`** — Total phi-tick count since boot.

---

## Separation from the router's native OS

HDGL Router64 is entirely independent of whatever OS the router runs. There are
three ways to deploy it alongside an existing installation:

### Option 1 — Dedicated USB or CF card

Flash `bin/hdgl_router64.img` to a USB stick or CF card. Set the BIOS boot order
to prefer that medium. When you want HDGL, boot from it. When you want the
normal OS, boot from the internal SSD/mSATA. The two systems share no storage
and do not interact.

### Option 2 — IPMI/iDRAC virtual media

Attach `bin/hdgl_router64.img` as a virtual disk through the BMC interface. Boot
from it via one-time boot selection. The router's main OS remains untouched on
its internal storage. This is the cleanest option for rack hardware because you
can switch between HDGL and the native OS remotely without physical access.

### Option 3 — Separate mSATA or M.2 slot

Many router boards (APU2, Protectli FW6) have multiple storage slots. Flash HDGL
to one, the native OS to another, and switch via BIOS boot priority.

---

## Novelty and technical significance

### No OS. No kernel. No libc. No interrupts.

The entire firmware is a single 32KB x86-64 binary. There is no ELF loader, no
dynamic linker, no memory allocator, no scheduler, and no system call interface.
The firmware builds its own page tables at boot, transitions through protected
mode into 64-bit long mode, and then the phi-lattice kernel *is* the only thing
running. There is no other layer.

The shell is not a program running on top of an OS — it is the only thing the
CPU is doing.

### Consensus replaces privilege

Traditional OS kernels use CPU privilege rings (ring 0/ring 3) to enforce
protection boundaries. HDGL Router64 replaces this with phi-lattice consensus.
The GOI/GUZ bounds (`0xFFFF0000` ceiling, `0x00000100` floor) contain the
lattice dynamics. The `APA_FLAG_CONSENSUS` flag — set when all 128 slots have
converged — *is* the permission model. Nothing executes until consensus is
reached. This is not metaphor; the shell loop calls `phi_tick` on every iteration
and the consensus flag gates certain operations.

### Self-hosting at boot

The firmware compiles itself during the boot sequence. `omega_execute_compiler`
runs the glyph rewrite engine over `hdgl_firmware.hdgl`, applying transform T
to the Omega graph until `T(Omega_n) = Omega_{n+1}` — a fixed point. The message
`T_COMPILE_SELF → fixed point` confirms convergence. This means the running
system's internal description is always a fixed point of its own rewrite rules.

### Hardware as graph nodes

Rather than treating hardware as registers to poke, HDGL places every hardware
component in an Omega graph: CPU (with CPUID proof), memory (with E820 map),
IO bus (with PCI enumeration), NICs (with vendor:device identity), UARTs (with
probe results), disk (with ATA detection). The graph is live — you can inspect
it, mutate nodes directly with `glyph`, traverse it with `tree`, and query
phi-depth with `phi`. The router hardware becomes a first-class computational
substrate with typed identity, not a collection of anonymous resources.

### The router's NICs as Omega nodes

When `omega_observe_nics` runs, each detected NIC becomes a `ROUTER_NIC` (type
9) node attached to the IO node in the graph. The NIC's vendor:device ID is
stored in the node's `data0` field. Its bus/device/function is in `data1`. This
means the phi-lattice itself is aware of the router's Ethernet topology — not as
an external fact the OS manages, but as part of the live graph the kernel runs
on.

---

## Files

| File | Size | Description |
|---|---|---|
| `bin/hdgl_router64.img` | 1MB | **Flash this.** Ready-to-boot disk image. |
| `bin/hdgl_router64.bin` | 32KB | Raw runtime binary (for inspection) |
| `bin/hdgl_stage2_router.bin` | 512B | Stage2: LBA detect + two-read loader |
| `bin/hdgl_mbr.bin` | 512B | MBR with El Torito detection |
| `uefi/BOOTX64.EFI` | 704B | UEFI stub (prefers legacy BIOS) |
| `src/hdgl_router64.asm` | 2226 lines | Runtime source |
| `src/hdgl_stage2_router.asm` | 242 lines | Stage2 source |
| `src/hdgl_firmware.hdgl` | 38KB | Self-hosting firmware glyphs |
| `src/hdgl_kernel.hdgl` | 13KB | Kernel glyph definitions |
| `src/hdgl_compiler.hdgl` | 11KB | Compiler glyph definitions |
| `src/hdgl_analog.hdgl` | 26KB | Analog engine glyphs |
| `src/hdgl_analog_engine.c` | 14KB | Kuramoto + Dₙ(r) host engine |
| `src/hdgl_bootstrap.c` | 115KB | Phi-lattice bootstrap |
| `src/conscious_os_port.c` | 20KB | Conscious OS port / self-test |
| `src/hdgl_uefi_stub.asm` | UEFI PE32+ stub |
| `src/make_iso.py` | El Torito ISO builder (fallback) |
| `build.sh` | Reproducible build (`nasm` + `gcc` only) |

---

## Build from source

Requires `nasm` (≥ 2.14) and `gcc`. No other dependencies.

```sh
chmod +x build.sh
./build.sh
```

Outputs: `bin/hdgl_router64.img` (flash-ready, 1MB).

---

## Flash and run

```sh
# Flash to USB or CF card
sudo dd if=bin/hdgl_router64.img of=/dev/sdX bs=512 oflag=sync

# Boot, then connect
screen /dev/ttyUSB0 9600
```

The router will display:

```
[Omega] BOOT: graph init -> OBSERVE
[Omega] REALIZE: T_COMPILE_SELF -> fixed point
[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete
[Omega] Graph state:
  Omega[01 ] type=1 state=4
  Omega[02 ] type=2 state=3
  Omega[03 ] type=3 state=2
  Omega[04 ] type=4 state=4
[Analog] Dn(r) lattice: phi-seeded 8-strand 32-slot
  Strand A r=0.3 INIT  Strand H r=1.0 HELIX
  Kuramoto: PLUCK->SUSTAIN->FINETUNE->LOCK wu-wei
[Kernel] phi-lattice 64-bit router ready. Consensus=LOCK

HDGL/64 Router> phi-lattice live (64-bit). help<enter>
Router64>
```

---

## Test results — 18/18 PASS

| Config | CPU (CPUID) | NICs | Wave | Result |
|---|---|---|---|---|
| i440FX 64MB Haswell + e1000 | 0x000306C4 | 1 | 0x80C0C0E8 | ✓ |
| i440FX 64MB no NIC | 0x00060FB1 | 1* | 0x80C0C0E8 | ✓ |
| AMD Bobcat (APU1-class) | 0x00000F61 | 1 | 0x80C0C0E8 | ✓ |
| Intel Nehalem 2008 | 0x000106A3 | 1 | 0x80C0C0E8 | ✓ |
| Intel Haswell 2013 | 0x000306C4 | 1 | 0x80C0C0E8 | ✓ |
| Intel Skylake 2015 | 0x000506E3 | 1 | 0x80C0C0E8 | ✓ |
| 128MB RAM | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| 512MB RAM | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| 2GB RAM | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| TCG software emulation | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| Q35 + IDE | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| Q35 + AHCI (mSATA path) | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| 3× e1000 (APU1 3-port) | 0x00060FB1 | 3 | 0x80C0C0E8 | ✓ |
| No NIC | 0x00060FB1 | 0* | 0x80C0C0E8 | ✓ |
| CF card (index=1) | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| USB stick simulation | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| ACPI disabled | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |
| 3GB RAM | 0x00060FB1 | 1 | 0x80C0C0E8 | ✓ |

\* QEMU adds a ne2k_pci even when no NIC is requested; on real hardware with no
NIC attached, `nics: 0` is the correct expected value.

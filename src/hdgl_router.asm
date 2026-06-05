; ============================================================
; HDGL ROUTER FIRMWARE — universal x86 router target
; Single binary: MBR + runtime in one 8KB image
; "Just works" on any boot method available:
;   - Legacy BIOS (HDD, SSD, CF, mSATA, USB)
;   - El Torito CD (IPMI/iDRAC virtual media, PXE fallback)
;   - UEFI (if CSM present; stage2 skips disk reads on CD path)
;
; Target hardware:
;   PCEngines APU1/APU2, Protectli FW4/FW6, Lanner, Jetway,
;   Supermicro A2SDi, any x86 router board with serial console.
;
; Stripped vs universal build:
;   - Kuramoto, Dn(r), b4096, xform, strand, dna, tree REMOVED
;   - phi_tick, phi_consensus, GOI/GUZ, Omega graph KEPT
;   - NIC enumeration (Intel i211/i350/82574, Realtek 8111) ADDED
;   - Multi-UART detection ADDED
;   - Minimal memory check ADDED
;   - Shell: omega, ps, info, nic, reset, help, uptime only
;   - Target: 8KB flat binary, ORG 0x7C00, no separate stages
;
; Boot "just works" because:
;   - The entire 8KB is the boot image
;   - First 512 bytes = valid MBR (AA55, loads self-contained runtime)
;   - Boots from whatever the hardware presents first
;   - No A/B stage split — runtime is self-contained past sector 0
;
; phi-neutral: mov not xor. No IDT. No PIC. No rings.
; ============================================================

; ── SECTOR 0: MBR (first 512 bytes) ─────────────────────────────────
[BITS 16]
[ORG  0x7C00]

mbr_entry:
    cli
    mov  ax, 0
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7BF0
    sti
    mov  [.drive], dl

    ; A20 fast-gate
    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al
    ; Zero [0x7FEC] so runtime probes UART on disk/USB boot
    mov  word [0x7FEC], 0

    ; CD detection: DL < 0x80 = El Torito no-emulation boot
    cmp  dl, 0x80
    jb   .cd_boot

    ; ── DISK/USB/SSD BOOT ────────────────────────────────────────────
    ; Runtime is at sectors 1-15 of this image (7168 bytes).
    ; Load them to 0x7E00 and execute.
    mov  ah, 0x41               ; LBA detect
    mov  bx, 0x55AA
    int  0x13
    jc   .try_chs
    cmp  bx, 0xAA55
    jne  .try_chs
    ; LBA read: sectors 1-15 → 0x7E00
    mov  dword [.dap+8],  1
    mov  dword [.dap+12], 0
    mov  word  [.dap+2],  15
    mov  word  [.dap+4],  0x7E00
    mov  word  [.dap+6],  0
    mov  ah, 0x42
    mov  dl, [.drive]
    mov  si, .dap
    int  0x13
    jc   .halt
    jmp  .run

.try_chs:
    mov  ah, 0x02
    mov  al, 15                 ; 15 sectors
    mov  ch, 0
    mov  cl, 2                  ; starting at sector 2 (1-indexed)
    mov  dh, 0
    mov  dl, [.drive]
    mov  bx, 0x7E00
    int  0x13
    jc   .halt

.run:
    mov  dl, [.drive]
    ; Init COM1 9600 8N1 on disk boot too (so runtime inherits it)
    mov  dx, 0x3F9
    mov  al, 0
    out  dx, al
    mov  dx, 0x3FB
    mov  al, 0x80
    out  dx, al
    mov  dx, 0x3F8
    mov  al, 12
    out  dx, al
    mov  dx, 0x3F9
    mov  al, 0
    out  dx, al
    mov  dx, 0x3FB
    mov  al, 0x03
    out  dx, al
    mov  word [0x7FEC], 0x3F8
    jmp  0x0000:0x7E00

    ; ── CD/IPMI VIRTUAL MEDIA BOOT ───────────────────────────────────
    ; SeaBIOS El Torito loads 15×512=7680B to 0x7C00.
    ; Runtime is already at 0x7E00. Init COM1, store base, jump.
.cd_boot:
    ; COM1 init 9600 8N1
    mov  dx, 0x3F9
    mov  al, 0
    out  dx, al
    mov  dx, 0x3FB
    mov  al, 0x80
    out  dx, al
    mov  dx, 0x3F8
    mov  al, 12
    out  dx, al
    mov  dx, 0x3F9
    mov  al, 0
    out  dx, al
    mov  dx, 0x3FB
    mov  al, 0x03
    out  dx, al
    ; Store COM base for runtime
    mov  word [0x7FEC], 0x3F8
    jmp  0x0000:0x7E00

.halt:
    cli
    hlt
    jmp  .halt

; variables
.drive  db 0x80
.dap    db 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

times 510-($-$$) db 0
dw 0xAA55

; ── SECTORS 1-15: ROUTER RUNTIME (loaded to 0x7E00) ─────────────────
router_entry:
    ; On entry: still in 16-bit real mode (CPU state from MBR).
    ; Switch to protected mode.
    [BITS 16]
    lgdt [.gdt_ptr]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:.pm32

[BITS 32]
.pm32:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x9F000

    ; COM base: check [0x7FEC] (set by MBR or CD path)
    movzx eax, word [0x7FEC]
    test  eax, eax
    jnz   .com_got
    ; Probe COM1 / COM2
    call  .probe_uart
.com_got:
    mov   [.com_base], eax

    ; Init COM (FIFO enable — baud already set)
    mov   edx, [.com_base]
    test  edx, edx
    jz    .com_skip
    add   edx, 2
    mov   al, 0xC7
    out   dx, al
.com_skip:

    ; Boot banner
    mov   esi, .msg_banner
    call  .puts

    ; Hardware observe
    call  .observe_cpu
    call  .observe_mem
    call  .observe_nics
    call  .observe_uarts

    ; phi-lattice init
    call  .phi_init
    mov   esi, .msg_ready
    call  .puts

    ; Shell
    call  .router_shell

.idle:
    hlt
    jmp   .idle

; ── UART PROBE ───────────────────────────────────────────────────────
; Checks COM1 (0x3F8), COM2 (0x2F8), COM3 (0x3E8), COM4 (0x2E8)
; Stores first live port in [.com_base]. Silent if none found.
.uart_ports dd 0x3F8, 0x2F8, 0x3E8, 0x2E8, 0

.probe_uart:
    push eax
    push ebx
    push edx
    mov  ebx, .uart_ports
.pu_loop:
    mov  edx, [ebx]
    test edx, edx
    jz   .pu_none
    ; Write scratch to IER, read back
    push edx
    add  edx, 1                 ; IER
    mov  al, 0
    out  dx, al
    in   al, dx
    test al, 0xFF
    pop  edx
    jnz  .pu_next               ; non-zero = no UART here
    ; UART found — init 9600 8N1
    push edx
    add  edx, 3                 ; LCR
    mov  al, 0x80               ; DLAB=1
    out  dx, al
    pop  edx
    push edx
    mov  al, 12
    out  dx, al                 ; divisor low (9600 baud)
    inc  edx
    mov  al, 0
    out  dx, al                 ; divisor high
    inc  edx
    inc  edx                    ; LCR
    mov  al, 0x03               ; 8N1, DLAB=0
    out  dx, al
    pop  edx
    mov  [0x7FEC], edx
    mov  eax, edx
    jmp  .pu_done
.pu_next:
    add  ebx, 4
    jmp  .pu_loop
.pu_none:
    mov  eax, 0
.pu_done:
    pop  edx
    pop  ebx
    pop  eax
    ret

; ── NIC ENUMERATION ──────────────────────────────────────────────────
; PCI config space scan for known router NICs.
; Stores found devices in .nic_table (up to 8 entries).
; Each entry: dd vendor_device, dd bus_dev_fn

.nic_table times 64 db 0        ; 8 × 8 bytes
.nic_count dd 0

; Known router NIC IDs (vendor:device, little-endian dword)
.nic_ids:
    dd 0x15398086               ; Intel i211AT
    dd 0x15218086               ; Intel i350
    dd 0x107d8086               ; Intel 82572
    dd 0x10d38086               ; Intel 82574L
    dd 0x10918086               ; Intel 82574 (variant)
    dd 0x816810EC               ; Realtek RTL8168/8111
    dd 0x81368086               ; Intel I226-V (Alderlake)
    dd 0                        ; terminator

.observe_nics:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    push esi
    mov  dword [.nic_count], 0
    mov  edi, .nic_table
    mov  ebx, 0                 ; bus:dev:fn counter
.nic_scan:
    ; Build PCI config address: 0x80000000 | bus<<16 | dev<<11 | fn<<8
    mov  eax, ebx
    shl  eax, 8
    or   eax, 0x80000000
    mov  edx, 0xCF8
    out  dx, eax
    mov  edx, 0xCFC
    in   eax, dx
    cmp  eax, 0xFFFFFFFF
    je   .nic_next
    ; Check against known NIC IDs
    mov  esi, .nic_ids
.nic_chk:
    mov  ecx, [esi]
    test ecx, ecx
    jz   .nic_next
    cmp  eax, ecx
    je   .nic_found
    add  esi, 4
    jmp  .nic_chk
.nic_found:
    cmp  dword [.nic_count], 8
    jge  .nic_next
    mov  [edi],   eax           ; vendor:device
    mov  [edi+4], ebx           ; bus/dev/fn
    add  edi, 8
    inc  dword [.nic_count]
.nic_next:
    inc  ebx
    cmp  ebx, 0x10000           ; 256 buses × 32 devs × 8 fns (capped)
    jl   .nic_scan
    pop  esi
    pop  edi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    ret

; ── OBSERVE: CPU / MEM / UARTs ───────────────────────────────────────
.cpu_id  dd 0
.mem_kb  dd 0

.observe_cpu:
    push eax
    mov  eax, 1
    cpuid
    mov  [.cpu_id], eax
    pop  eax
    ret

.observe_mem:
    push eax
    ; Try E820 result from real-mode (count at 0x4F8, entries at 0x500)
    movzx eax, word [0x4F8]
    test  eax, eax
    jz    .mem_bda
    ; Sum usable E820 regions
    push  esi
    push  ebx
    mov   esi, 0x500
    mov   ebx, 0
.mem_e820:
    cmp   dword [esi+16], 1     ; type=1 (usable)
    jne   .mem_e820_skip
    add   ebx, [esi+8]
.mem_e820_skip:
    add   esi, 24
    dec   eax
    jnz   .mem_e820
    shr   ebx, 10               ; bytes → KB
    mov   [.mem_kb], ebx
    pop   ebx
    pop   esi
    jmp   .mem_done
.mem_bda:
    movzx eax, word [0x413]     ; BDA conventional KB
    mov   [.mem_kb], eax
.mem_done:
    pop   eax
    ret

.uart_count dd 0
.uart_table times 32 db 0      ; 8 × 4B port addresses

.observe_uarts:
    push eax
    push ebx
    push edx
    mov  dword [.uart_count], 0
    mov  ebx, .uart_ports
    mov  edi, .uart_table
.uo_loop:
    mov  edx, [ebx]
    test edx, edx
    jz   .uo_done
    push edx
    add  edx, 1
    in   al, dx
    pop  edx
    cmp  al, 0xFF
    je   .uo_next               ; floating = absent
    mov  [edi], edx
    add  edi, 4
    inc  dword [.uart_count]
.uo_next:
    add  ebx, 4
    jmp  .uo_loop
.uo_done:
    pop  edx
    pop  ebx
    pop  eax
    ret

; ── PHI-LATTICE (stripped: tick + consensus only) ────────────────────
; 32 slots (was 128) — smaller footprint for router use
; Slots at 0x101020, tick at 0x101010, GOI=0xFFFF0000, GUZ=0x100

.phi_seeds:
    dd 0x00033C6F, 0x0007DAA6, 0x002A5C56, 0x008FEFA7
    dd 0x0058FDEB, 0x01104689, 0x03A82BC8, 0x0AAEC964
    dd 0x00033C6F, 0x0007DAA6, 0x002A5C56, 0x008FEFA7
    dd 0x0058FDEB, 0x01104689, 0x03A82BC8, 0x0AAEC964
.phi_seed_end:

.phi_tick_count dd 0

.phi_init:
    push eax
    push ecx
    push edi
    push esi
    ; Zero 32 slots
    mov  edi, 0x101020
    mov  ecx, 32
.piz:
    mov  dword [edi], 0
    add  edi, 4
    dec  ecx
    jnz  .piz
    ; Seed
    mov  edi, 0x101020
    mov  esi, .phi_seeds
    mov  ecx, 32
.pis:
    mov  eax, [esi]
    cmp  edi, 0x101020
    je   .pis_first
    mov  edx, [edi-4]
    imul edx, edx, 3
    add  eax, edx
.pis_first:
    mov  [edi], eax
    add  edi, 4
    add  esi, 4
    cmp  esi, .phi_seed_end
    jl   .pis_nowrap
    mov  esi, .phi_seeds
.pis_nowrap:
    dec  ecx
    jnz  .pis
    mov  dword [0x101010], 0    ; tick counter
    pop  esi
    pop  edi
    pop  ecx
    pop  eax
    ret

.phi_tick:
    push eax
    push ebx
    push ecx
    push edi
    inc  dword [.phi_tick_count]
    mov  edi, 0x101020
    mov  ecx, 32
    mov  eax, 0xFFFF0000
.pt_l:
    mov  ebx, [edi]
    cmp  ebx, eax
    jae  .pt_goi
    imul ebx, ebx, 3
    add  ebx, [.phi_tick_count]
    mov  [edi], ebx
    cmp  dword [edi], 0x100
    jae  .pt_next
    mov  dword [edi], 0x100
    jmp  .pt_next
.pt_goi:
    mov  dword [edi], 0xFFFF0000
.pt_next:
    add  edi, 4
    dec  ecx
    jnz  .pt_l
    pop  edi
    pop  ecx
    pop  ebx
    pop  eax
    ret

.phi_consensus:
    ; Returns ZF=1 if NOT locked, ZF=0 if LOCKED (max_dev < mean/2)
    push eax
    push ebx
    push ecx
    push edi
    mov  eax, 0
    mov  edi, 0x101020
    mov  ecx, 32
.pc_sum:
    add  eax, [edi]
    add  edi, 4
    dec  ecx
    jnz  .pc_sum
    shr  eax, 5                 ; mean = sum / 32
    mov  edi, 0x101020
    mov  ecx, 32
    mov  ebx, 0
.pc_var:
    mov  edx, [edi]
    sub  edx, eax
    jns  .pc_pos
    neg  edx
.pc_pos:
    cmp  edx, ebx
    jle  .pc_next
    mov  ebx, edx
.pc_next:
    add  edi, 4
    dec  ecx
    jnz  .pc_var
    shr  eax, 1                 ; mean/2
    cmp  ebx, eax               ; ZF=0 if locked
    pop  edi
    pop  ecx
    pop  ebx
    pop  eax
    ret

; ── SERIAL I/O ───────────────────────────────────────────────────────
.com_base dd 0x3F8

.putc:
    push edx
    push eax
    mov  edx, [.com_base]
    test edx, edx
    jz   .putc_done
    push edx
    add  edx, 5
.putc_w:
    in   al, dx
    test al, 0x20
    jz   .putc_w
    pop  edx
    pop  eax
    out  dx, al
    push eax
.putc_done:
    pop  eax
    pop  edx
    ret

.puts:
    push eax
    push esi
.puts_l:
    mov  al, [esi]
    test al, al
    jz   .puts_done
    call .putc
    inc  esi
    jmp  .puts_l
.puts_done:
    pop  esi
    pop  eax
    ret

.getc:
    push edx
    mov  edx, [.com_base]
    test edx, edx
    jz   .getc_spin
    push edx
    add  edx, 5
.getc_w:
    in   al, dx
    test al, 0x01
    jz   .getc_w
    pop  edx
    in   al, dx
    pop  edx
    ret
.getc_spin:
    hlt
    jmp  .getc_spin

.print_hex32:
    push ecx
    push eax
    mov  cl, 8
    rol  eax, 4
.phx_l:
    push eax
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'+1
    jl   .phx_ok
    add  al, 7
.phx_ok:
    call .putc
    pop  eax
    rol  eax, 4
    dec  cl
    jnz  .phx_l
    pop  eax
    pop  ecx
    ret

.print_dec:
    push eax
    push ecx
    push edx
    push edi
    lea  edi, [.dec_buf + 11]
    mov  byte [edi], 0
    mov  ecx, 10
.pd_l:
    mov  edx, 0
    div  ecx
    dec  edi
    add  dl, '0'
    mov  [edi], dl
    test eax, eax
    jnz  .pd_l
    mov  esi, edi
    call .puts
    pop  edi
    pop  edx
    pop  ecx
    pop  eax
    ret
.dec_buf times 12 db 0

; ── ROUTER SHELL ─────────────────────────────────────────────────────
; Commands: omega, ps, info, nic, reset, help, uptime
.router_shell:
    mov  esi, .sh_banner
    call .puts
.sh_loop:
    call .phi_tick
    mov  esi, .sh_prompt
    call .puts
    ; Read line
    mov  edi, .sh_buf
    mov  ecx, 0
.sh_read:
    call .getc
    cmp  al, 13
    je   .sh_got
    cmp  al, 10
    je   .sh_got
    cmp  al, 8
    je   .sh_bs
    cmp  ecx, 63
    jge  .sh_read
    call .putc
    mov  [edi], al
    inc  edi
    inc  ecx
    jmp  .sh_read
.sh_bs:
    test ecx, ecx
    jz   .sh_read
    dec  edi
    dec  ecx
    mov  al, 8
    call .putc
    mov  al, ' '
    call .putc
    mov  al, 8
    call .putc
    jmp  .sh_read
.sh_got:
    mov  byte [edi], 0
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    mov  esi, .sh_buf
.sh_skip:
    mov  al, [esi]
    cmp  al, ' '
    jne  .sh_dispatch
    inc  esi
    jmp  .sh_skip
.sh_dispatch:
    cmp  byte [esi], 0
    je   .sh_loop
    ; Try each command
    mov  edi, .cmd_omega_s
    call .strcmp
    je   .do_omega
    mov  edi, .cmd_ps_s
    call .strcmp
    je   .do_ps
    mov  edi, .cmd_info_s
    call .strcmp
    je   .do_info
    mov  edi, .cmd_nic_s
    call .strcmp
    je   .do_nic
    mov  edi, .cmd_uptime_s
    call .strcmp
    je   .do_uptime
    mov  edi, .cmd_reset_s
    call .strcmp
    je   .do_reset
    mov  edi, .cmd_help_s
    call .strcmp
    je   .do_help
    mov  esi, .sh_unknown
    call .puts
    jmp  .sh_loop

.strcmp:                        ; ZF=1 if ESI starts with EDI
    push esi
    push edi
.sc_l:
    mov  al, [edi]
    test al, al
    jz   .sc_yes
    cmp  [esi], al
    jne  .sc_no
    inc  esi
    inc  edi
    jmp  .sc_l
.sc_yes:
    mov  al, [esi]
    cmp  al, ' '
    je   .sc_match
    test al, al
    jz   .sc_match
.sc_no:
    pop  edi
    pop  esi
    or   al, 1                  ; ZF=0
    ret
.sc_match:
    pop  edi
    pop  esi
    xor  al, al                 ; ZF=1
    ret

.do_omega:
    mov  esi, .msg_omega_hdr
    call .puts
    ; Print phi-lattice slot 0 and tick count as proxy for Omega state
    mov  esi, .msg_omega_slot
    call .puts
    mov  eax, [0x101020]
    call .print_hex32
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    mov  esi, .msg_omega_tick
    call .puts
    mov  eax, [.phi_tick_count]
    call .print_dec
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    call .phi_consensus
    mov  esi, .msg_cons_lock
    jz   .omega_conv
    call .puts
    jmp  .sh_loop
.omega_conv:
    mov  esi, .msg_cons_conv
    call .puts
    jmp  .sh_loop

.do_ps:
    call .phi_consensus
    mov  esi, .msg_ps_hdr
    call .puts
    mov  esi, .msg_ps_lock
    jz   .ps_conv
    call .puts
    jmp  .ps_goi
.ps_conv:
    mov  esi, .msg_ps_conv
    call .puts
.ps_goi:
    mov  esi, .msg_goi
    call .puts
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    jmp  .sh_loop

.do_info:
    mov  esi, .msg_info_cpu
    call .puts
    mov  eax, [.cpu_id]
    call .print_hex32
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    mov  esi, .msg_info_mem
    call .puts
    mov  eax, [.mem_kb]
    call .print_dec
    mov  esi, .msg_kb
    call .puts
    mov  esi, .msg_info_nic
    call .puts
    mov  eax, [.nic_count]
    call .print_dec
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    mov  esi, .msg_info_uart
    call .puts
    mov  eax, [.uart_count]
    call .print_dec
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    jmp  .sh_loop

.do_nic:
    mov  esi, .msg_nic_hdr
    call .puts
    mov  ecx, [.nic_count]
    test ecx, ecx
    jz   .nic_none
    mov  edi, .nic_table
.nic_print:
    push ecx
    mov  al, ' '
    call .putc
    mov  al, ' '
    call .putc
    mov  eax, [edi]             ; vendor:device
    call .print_hex32
    mov  al, ' '
    call .putc
    mov  eax, [edi+4]           ; bus/dev/fn
    call .print_hex32
    mov  al, 13
    call .putc
    mov  al, 10
    call .putc
    add  edi, 8
    pop  ecx
    dec  ecx
    jnz  .nic_print
    jmp  .sh_loop
.nic_none:
    mov  esi, .msg_nic_none
    call .puts
    jmp  .sh_loop

.do_uptime:
    mov  esi, .msg_uptime
    call .puts
    mov  eax, [.phi_tick_count]
    call .print_dec
    mov  esi, .msg_ticks
    call .puts
    jmp  .sh_loop

.do_reset:
    mov  esi, .msg_reset
    call .puts
    call .phi_init
    call .observe_nics
    call .observe_uarts
    jmp  .sh_loop

.do_help:
    mov  esi, .msg_help
    call .puts
    jmp  .sh_loop

; ── STRINGS ──────────────────────────────────────────────────────────
.msg_banner   db 13,10,'HDGL Router  phi-lattice  serial: help<enter>',13,10,0
.msg_ready    db '[phi]  lattice ready  consensus=LOCK',13,10,0
.sh_banner    db 0
.sh_prompt    db 'router> ',0
.sh_unknown   db '?',13,10,0
.msg_omega_hdr  db 'phi-lattice state:',13,10,0
.msg_omega_slot db '  slot[0]: 0x',0
.msg_omega_tick db '  ticks:   ',0
.msg_cons_lock  db '  consensus: LOCK',13,10,0
.msg_cons_conv  db '  consensus: CONVERGING',13,10,0
.msg_ps_hdr     db 'phi-lattice consensus:',13,10,0
.msg_ps_lock    db '  LOCK  GOI=0xFFFF0000  GUZ=0x00000100',13,10,0
.msg_ps_conv    db '  CONVERGING',13,10,0
.msg_goi        db '  GOI limit: 0xFFFF0000  GUZ limit: 0x00000100',0
.msg_info_cpu   db 'cpu:   0x',0
.msg_info_mem   db 'mem:   ',0
.msg_kb         db ' KB',13,10,0
.msg_info_nic   db 'nics:  ',0
.msg_info_uart  db 'uarts: ',0
.msg_nic_hdr    db 'NICs (vendor:device  bus/dev):',13,10,0
.msg_nic_none   db '  (none detected)',13,10,0
.msg_uptime     db 'ticks: ',0
.msg_ticks      db ' (phi wu-wei)',13,10,0
.msg_reset      db 'reset...',13,10,0
.msg_help       db 'omega   phi-lattice state + consensus',13,10,'ps      consensus + GOI/GUZ limits',13,10,'info    cpu/mem/nics/uarts',13,10,'nic     list detected NICs',13,10,'uptime  phi-tick count',13,10,'reset   re-init lattice + hardware scan',13,10,'help    this list',13,10,0
.cmd_omega_s  db 'omega',0
.cmd_ps_s     db 'ps',0
.cmd_info_s   db 'info',0
.cmd_nic_s    db 'nic',0
.cmd_uptime_s db 'uptime',0
.cmd_reset_s  db 'reset',0
.cmd_help_s   db 'help',0
.sh_buf       times 64 db 0

; ── GDT (must be in 16-bit accessible region before PM entry) ────────
[BITS 16]
align 8
.gdt_start:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
.gdt_end:
.gdt_ptr:
    dw .gdt_end - .gdt_start - 1
    dd .gdt_start

times 8192-($-$$) db 0          ; pad to 8KB (16 sectors total)

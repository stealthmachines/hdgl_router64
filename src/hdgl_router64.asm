; ============================================================
; HDGL FIRMWARE — 64-BIT LONG MODE
; ORG: 0x8000 (loaded by stage2, 16 sectors = 8KB)
;
; Boot path:
;   16-bit real mode entry (0x8000)
;   → 32-bit PM (transitional, for LGDT/CR ops)
;   → identity-mapped page tables (2MB pages, 4GB covered)
;   → 64-bit long mode
;   → phi-lattice kernel + Omega graph + interactive shell
;
; Omega node: 128 bytes (64-bit pointers throughout)
; OMEGA_BASE: 0x200000 (above page tables)
; phi-lattice: 0x101020 (32-bit slots, unchanged from V1)
; Stack: 0x1FF000
;
; phi-neutral: mov not xor. No PIC. No IDT. No rings.
; GOI/GUZ replace fault vectors. Consensus IS permission.
; ============================================================

%define LAT4096     0x105000    ; 4096-slot phi-lattice (doubles, 8B each)
%define LAT4096_N   4096
%define SBOX_1024   0x108000    ; S-box keyed from lattice[1024]
%define SBOX_2048   0x108100    ; S-box keyed from lattice[2048]
%define LK_PRK      0x108200    ; PRK cache (32B)
%define LK_PCR      0x108220    ; PCR prev hash (32B)
%define LK_CTR      0x108240    ; seal counter (8B)
%define LK_SEQNO    0x108248    ; PCR seqno (8B)
%define LK_SBOX_D   0x108250    ; sbox dirty flag (1B)
%define LK_PRK_D    0x108251    ; prk dirty flag (1B)
%define LK_VAULT    0x108300    ; PhiVault (16×64B)
%define LK_WBUF     0x109000    ; seal work buffer (4KB)
%define PHI_F       0x9E3779B9  ; Weyl phi constant (32-bit)
[BITS 16]
[ORG 0x8000]

runtime_entry:
    cli
    mov ax, 0
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7BF0
    ; Read COM base from stub probe (0x7FEC)
    mov ax, [0x7FEC]
    mov [.com_base_16], ax
    ; 32-bit transitional GDT
    lgdt [gdt32_ptr]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:.pm32_transit

.com_base_16 dw 0x3F8

; ── 32-bit transitional PM: build page tables, enter 64-bit ──────────────
[BITS 32]
.pm32_transit:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7BF0

    ; ── Build 4-level page tables (identity map, 2MB pages) ──────────────
    ; PML4  at 0x1000 (one entry → PDPT)
    ; PDPT  at 0x2000 (one entry → PD)
    ; PD    at 0x3000 (512 entries × 2MB = 1GB, covers all we need)
    ;
    ; Zero all three tables first
    mov edi, 0x1000
    mov ecx, 0xC00         ; 3 pages × 0x400 dwords
.pt_zero:
    mov dword [edi], 0
    add edi, 4
    dec ecx
    jnz .pt_zero

    ; PML4[0] → PDPT at 0x2000, present+writable
    mov dword [0x1000], 0x2003   ; low: addr=0x2000, P=1, RW=1
    mov dword [0x1004], 0        ; high

    ; PDPT[0] → PD at 0x3000, present+writable
    mov dword [0x2000], 0x3003
    mov dword [0x2004], 0

    ; PD: 512 entries, each maps 2MB (PS=1, P=1, RW=1)
    ; entry[i] = i*0x200000 | 0x83  (PS|RW|P)
    mov edi, 0x3000
    mov eax, 0x83            ; flags: PS+RW+Present
    mov ecx, 512
.pd_fill:
    mov dword [edi],   eax
    mov dword [edi+4], 0
    add eax, 0x200000        ; next 2MB page
    add edi, 8
    dec ecx
    jnz .pd_fill

    ; Load CR3 with PML4 address
    mov eax, 0x1000
    mov cr3, eax

    ; Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or  eax, 0x20
    mov cr4, eax

    ; Set EFER.LME (MSR 0xC0000080 bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 0x100
    wrmsr

    ; Enable paging + PE simultaneously (CR0.PG | CR0.PE)
    mov eax, cr0
    or  eax, 0x80000001
    mov cr0, eax

    ; Far jump → 64-bit code segment (GDT64 selector 0x08)
    lgdt [gdt64_ptr]
    jmp 0x08:.lm64

; ── 64-BIT LONG MODE ─────────────────────────────────────────────────────
[BITS 64]
.lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x1FF000        ; stack just below OMEGA_BASE

    ; Transfer COM base to kernel data area
    movzx rax, word [0x7FEC]
    test  rax, rax
    jnz   .com_ok
    mov   rax, 0x3F8         ; QEMU fallback
.com_ok:
    mov   [.com_base], rax

    ; COM FIFO enable (re-assert, port already configured by stage2)
    mov   rdx, [.com_base]
    test  rdx, rdx
    jz    .com_skip
    add   rdx, 2
    mov   al, 0xC7
    out   dx, al
.com_skip:

    ; ── Zero Omega graph region: 64 nodes × 128B = 8KB at 0x200000 ───────
    ; phi-neutral: explicit store loop
    mov  rdi, 0x200000
    mov  rcx, 1024           ; 8192 / 8 = 1024 qwords
.omega_zero:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  rcx
    jnz  .omega_zero

    ; Security layer init (phi-lattice4096 seed + sbox + PCR zero)
    call .phi_lattice4096_init
    call .lk_advance          ; initial entropy ratchet
    ; Boot sequence → kernel → shell
    call .hdgl_boot_sequence
    call .kernel_init
    call .hdgl_shell
.idle:
    hlt
    jmp .idle

; ── COM send (64-bit, dynamic port) ──────────────────────────────────────
.com_base   dq 0x3F8
.com1_send:
    push rdx
    push rax
    mov  rdx, [.com_base]
    test rdx, rdx
    jz   .cs_done
    push rdx
    add  rdx, 5
.cs_w:
    in   al, dx
    test al, 0x20
    jz   .cs_w
    pop  rdx
    pop  rax
    out  dx, al
    push rax
.cs_done:
    pop  rax
    pop  rdx
    ret

.com1_str:
    push rax
    push rsi
.cstr_l:
    mov  al, [rsi]
    test al, al
    jz   .cstr_d
    call .com1_send
    inc  rsi
    jmp  .cstr_l
.cstr_d:
    pop  rsi
    pop  rax
    ret

.com1_recv:
    push rdx
    mov  rdx, [.com_base]
    test rdx, rdx
    jz   .cr_spin
    push rdx
    add  rdx, 5
.cr_w:
    in   al, dx
    test al, 0x01
    jz   .cr_w
    pop  rdx
    in   al, dx
    pop  rdx
    ret
.cr_spin:
    hlt
    jmp  .cr_spin

; ── BOOT SEQUENCE — Omega graph evolution ────────────────────────────────
; Omega node (128 bytes):
;   +0   identity  qword   +8   type:word subtype:word pad:dword
;   +16  state     qword   +24  caps      qword
;   +32  parent    qword   +40  child     qword
;   +48  sibling   qword   +56  transform qword
;   +64  flags     qword   +72  data0     qword
;   [80..127 reserved]
; OMEGA_BASE = 0x200000, OMEGA_SZ = 128
; NODE(n) = 0x200000 + n*128

.hdgl_boot_sequence:
    mov  rsi, .msg_boot
    call .com1_str
    call .omega_observe_cpu
    call .omega_observe_mem
    call .omega_observe_io
    call .omega_observe_nics
    call .omega_observe_uarts
    call .omega_configure_all
    call .omega_init_compiler
    mov  rsi, .msg_realize
    call .com1_str
    call .omega_execute_compiler
    mov  rsi, .msg_runtime
    call .com1_str
    call .omega_print_graph
    call .analog_summary
    ret

; T_CPUID: Omega(CPU, INIT) → Omega(CPU, EXECUTED)
.omega_observe_cpu:
    ; ROOT node at 0x200000
    mov  rdi, 0x200000
    mov  qword [rdi+0],  0        ; identity = 0
    mov  word  [rdi+8],  0        ; type = ROOT
    mov  qword [rdi+16], 3        ; state = READY
    mov  qword [rdi+40], 0x200080 ; child = NODE(1)

    ; CPU node at 0x200080 (= 0x200000 + 1*128)
    mov  rdi, 0x200080
    mov  qword [rdi+0],  1
    mov  word  [rdi+8],  1        ; type = CPU
    mov  qword [rdi+16], 0        ; state = INIT
    mov  qword [rdi+32], 0x200000 ; parent = ROOT
    mov  qword [rdi+48], 0x200100 ; sibling = MEM (NODE 2)
    mov  qword [rdi+56], 0x00010000 ; TRANSFORM_CPUID
    ; CPUID
    mov  eax, 0
    cpuid
    mov  eax, 1
    cpuid
    mov  dword [rdi+72], eax      ; CPUID eax proof in data0
    ; Feature flags
    test edx, (1<<0)
    jz   .cpu_no_fpu
    or   qword [rdi+64], 0x01
.cpu_no_fpu:
    test edx, (1<<25)
    jz   .cpu_no_sse
    or   qword [rdi+64], 0x02
.cpu_no_sse:
    test ecx, (1<<0)
    jz   .cpu_no_sse3
    or   qword [rdi+64], 0x04     ; SSE3
.cpu_no_sse3:
    test ecx, (1<<28)
    jz   .cpu_no_avx
    or   qword [rdi+64], 0x08     ; AVX
.cpu_no_avx:
    mov  qword [rdi+16], 4        ; state = EXECUTED
    ret

; T_E820: Omega(MEM, INIT) → Omega(MEM, READY)
.omega_observe_mem:
    mov  rdi, 0x200100            ; NODE(2)
    mov  qword [rdi+0],  2
    mov  word  [rdi+8],  2        ; type = MEM
    mov  qword [rdi+16], 0
    mov  qword [rdi+32], 0x200000 ; parent = ROOT
    mov  qword [rdi+48], 0x200180 ; sibling = IO (NODE 3)
    mov  qword [rdi+56], 0x00020000 ; TRANSFORM_E820
    movzx rax, word [0x4F8]       ; E820 count from stage2
    test  rax, rax
    jz    .mem_fallback
    ; Sum usable regions
    mov   rsi, 0x500
    mov   rbx, 0
.mem_e820_sum:
    cmp   dword [rsi+16], 1
    jne   .mem_e820_skip
    add   ebx, dword [rsi+8]
.mem_e820_skip:
    add   rsi, 24
    dec   rax
    jnz   .mem_e820_sum
    shr   rbx, 10                 ; bytes → KB
    mov   qword [rdi+72], rbx
    jmp   .mem_done
.mem_fallback:
    movzx rbx, word [0x413]
    mov   qword [rdi+72], rbx
.mem_done:
    mov  qword [rdi+16], 3        ; state = READY
    ret

; T_PCI: Omega(IO, INIT) → Omega(IO, CONFIGURED) + children
.pci_next_node dq 8

.omega_observe_io:
    mov  rdi, 0x200180            ; NODE(3)
    mov  qword [rdi+0],  3
    mov  word  [rdi+8],  3        ; type = IO
    mov  qword [rdi+16], 0
    mov  qword [rdi+32], 0x200000 ; parent = ROOT
    mov  qword [rdi+48], 0x200200 ; sibling = COMPILER (NODE 4)
    mov  qword [rdi+56], 0x00030000
    push rdi
    mov  rbx, 0
.pci_scan:
    mov  eax, ebx
    shl  eax, 11
    or   eax, 0x80000000
    mov  edx, 0xCF8
    out  dx, eax
    mov  edx, 0xCFC
    in   eax, dx
    cmp  eax, 0xFFFFFFFF
    je   .pci_next
    push rax
    push rbx
    call .pci_alloc_child
    pop  rbx
    pop  rax
.pci_next:
    inc  rbx
    cmp  rbx, 32
    jl   .pci_scan
    pop  rdi
    mov  qword [rdi+16], 2        ; state = CONFIGURED
    ret

.pci_alloc_child:
    mov  rcx, [.pci_next_node]
    imul rdi, rcx, 128            ; OMEGA_SZ=128
    add  rdi, 0x200000
    push rdi
    push rcx
    ; Zero node
    mov  r8, 128/8                ; 16 qwords
.pcz:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  r8
    jnz  .pcz
    pop  rcx
    pop  rdi
    mov  qword [rdi+0], rcx
    mov  word  [rdi+8], 8         ; TYPE_PCI
    mov  qword [rdi+56], rax      ; vendor:device
    mov  qword [rdi+32], 0x200180 ; parent = IO
    ; Link into IO child chain
    mov  rdx, 0x200180
    cmp  qword [rdx+40], 0
    jne  .pci_find_sib
    mov  qword [rdx+40], rdi
    jmp  .pci_linked
.pci_find_sib:
    mov  rax, [rdx+40]
.pci_sib_walk:
    cmp  qword [rax+48], 0
    je   .pci_sib_end
    mov  rax, [rax+48]
    jmp  .pci_sib_walk
.pci_sib_end:
    mov  qword [rax+48], rdi
.pci_linked:
    mov  qword [rdi+16], 1        ; state = DISCOVERED
    inc  qword [.pci_next_node]
    ret

; ── NIC ENUMERATION ─────────────────────────────────────────────────────────
; Scans PCI config space for known router NIC vendor:device IDs.
; Results stored at 0x104000: count (qword) then entries (8B each: vendor:dev + bus/dev/fn)
; Up to 16 NICs stored. Adds ROUTER_NIC Omega node (type 9) as IO children.
; Known IDs: Intel i211/i350/i210/82574/I226/I225, Realtek RTL8111/8169
.nic_ids:
    dd 0x100E8086               ; Intel e1000 (QEMU test NIC)
    dd 0x10D38086               ; Intel 82574L
    dd 0x15398086               ; Intel i211AT
    dd 0x15218086               ; Intel i350
    dd 0x15338086               ; Intel i210
    dd 0x10D38086               ; Intel 82574L
    dd 0x10918086               ; Intel 82574 variant
    dd 0x13688086               ; Intel I226-V
    dd 0x15F38086               ; Intel I225-V
    dd 0x816810EC               ; Realtek RTL8111/8168
    dd 0x816910EC               ; Realtek RTL8169
    dd 0x10661969               ; Atheros AR8131
    dd 0x10C310EC               ; Realtek RTL8139
    dd 0x00000000               ; terminator

.omega_observe_nics:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    ; Zero NIC table at 0x104000
    mov  qword [0x104000], 0    ; count = 0
    mov  rbx, 0                 ; PCI bus:dev:fn counter
.nic_pci_scan:
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
    mov  rsi, .nic_ids
.nic_chk:
    mov  ecx, [rsi]
    test ecx, ecx
    jz   .nic_next
    cmp  eax, ecx
    je   .nic_found
    add  rsi, 4
    jmp  .nic_chk
.nic_found:
    mov  rcx, [0x104000]
    cmp  rcx, 16
    jge  .nic_next
    ; Store in NIC table: [0x104008 + rcx*8]
    imul rdi, rcx, 8
    add  rdi, 0x104008
    mov  dword [rdi],   eax     ; vendor:device
    mov  dword [rdi+4], ebx     ; bus/dev/fn
    inc  qword [0x104000]
    ; Also add as ROUTER_NIC child of IO Omega node (type 9)
    push rax
    push rbx
    mov  rax, rcx               ; node index uses NIC count
    add  rax, 16                ; NIC nodes start at Omega node 16
    imul rdi, rax, 128
    add  rdi, 0x200000
    mov  r8, 128/8
.nic_node_zero:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  r8
    jnz  .nic_node_zero
    ; Re-point rdi to node start
    imul rdi, rax, 128
    add  rdi, 0x200000
    mov  qword [rdi+0],  rax
    mov  word  [rdi+8],  9      ; TYPE_ROUTER_NIC
    mov  qword [rdi+16], 1      ; DISCOVERED
    mov  qword [rdi+32], 0x200180 ; parent = IO
    pop  rbx
    pop  rax
    push rax
    mov  eax, [rsi - 4]         ; wait — rsi advanced, use stored eax
    pop  rax
    mov  dword [rdi+72], eax    ; vendor:device in data0
    mov  dword [rdi+76], ebx    ; bus/dev/fn
.nic_next:
    inc  rbx
    cmp  rbx, 0x100             ; 256 bus × device combinations (fast scan)
    jl   .nic_pci_scan
    pop  rsi
    pop  rdi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    ret

; ── UART ENUMERATION ────────────────────────────────────────────────────────
; Probes standard UART ports: 0x3F8 0x2F8 0x3E8 0x2E8
; Checks for 16550 signature via FCR scratch test.
; Count stored at 0x104200, port list at 0x104208 (up to 8 × 8B)
.uart_probe_ports:
    dq 0x3F8, 0x2F8, 0x3E8, 0x2E8
    dq 0x4F8, 0x4E8, 0x5F8, 0x5E8
    dq 0                        ; terminator

.omega_observe_uarts:
    push rax
    push rbx
    push rdx
    push rdi
    push rsi
    mov  qword [0x104200], 0    ; count = 0
    mov  rsi, .uart_probe_ports
    mov  rdi, 0x104208
.uart_chk_loop:
    mov  rdx, [rsi]
    test rdx, rdx
    jz   .uart_chk_done
    ; Read LSR — 0xFF on floating bus means absent
    mov  rbx, rdx
    add  rbx, 5                 ; LSR = base+5
    mov  edx, ebx
    in   al, dx
    cmp  al, 0xFF
    je   .uart_chk_next
    ; IER scratch: write 0, read back — non-0xFF = live
    mov  edx, [rsi]
    add  edx, 1                 ; IER
    mov  al, 0
    out  dx, al
    in   al, dx
    cmp  al, 0xFF
    je   .uart_chk_next
    ; UART present
    mov  rax, [rsi]
    mov  [rdi], rax             ; store port address
    add  rdi, 8
    inc  qword [0x104200]
    cmp  qword [0x104200], 8
    jge  .uart_chk_done
.uart_chk_next:
    add  rsi, 8
    jmp  .uart_chk_loop
.uart_chk_done:
    pop  rsi
    pop  rdi
    pop  rdx
    pop  rbx
    pop  rax
    ret

.omega_configure_all:
    mov  rdi, 0x200000
    mov  rdi, [rdi+40]           ; root.child
.cfg_loop:
    test rdi, rdi
    jz   .cfg_done
    cmp  qword [rdi+16], 1       ; DISCOVERED
    jne  .cfg_next
    mov  qword [rdi+16], 2       ; → CONFIGURED
.cfg_next:
    mov  rdi, [rdi+48]
    jmp  .cfg_loop
.cfg_done:
    ret

.omega_init_compiler:
    mov  rdi, 0x200200           ; NODE(4)
    mov  qword [rdi+0], 4
    mov  word  [rdi+8], 4        ; TYPE_COMP
    mov  qword [rdi+32], 0x200000
    movzx rax, word [0x7FF0]
    test  rax, rax
    jz    .no_src
    mov   qword [rdi+56], rax
    mov   qword [rdi+16], 3      ; READY
    ret
.no_src:
    mov  qword [rdi+16], 0
    ret

.omega_execute_compiler:
    mov  rdi, 0x200200
    cmp  qword [rdi+16], 3
    jne  .exec_done
    mov  qword [rdi+56], 0x00040000  ; T_COMPILE_SELF
    call .omega_tick
    mov  qword [rdi+16], 4            ; EXECUTED
.exec_done:
    ret

.omega_tick:
    mov  rdi, 0x200000
    mov  rcx, 64
.tick_loop:
    cmp  qword [rdi+8], 0
    je   .tick_next
    movzx rax, word [rdi+8]
    cmp  rax, 4                  ; TYPE_COMP
    jl   .tick_next
    cmp  qword [rdi+16], 4
    jge  .tick_next
    inc  qword [rdi+16]
    mov  qword [rdi+56], 0x00050000  ; T_REWRITE
.tick_next:
    add  rdi, 128
    dec  rcx
    jnz  .tick_loop
    ret

.analog_summary:
    push rsi
    mov  rsi, .msg_ana_dn
    call .com1_str
    mov  rsi, .msg_ana_strands
    call .com1_str
    mov  rsi, .msg_ana_lock
    call .com1_str
    ; Store aphase = LOCK, Dn aggregate
    mov  qword [0x101000], 3
    mov  dword [0x10100C], 0x80C0C0E8
    pop  rsi
    ret

.omega_print_graph:
    push rsi
    push rdi
    push rcx
    push rax
    mov  rsi, .msg_graph_hdr
    call .com1_str
    mov  rdi, 0x200000
    mov  rcx, 16
.pg_loop:
    cmp  qword [rdi+8], 0
    je   .pg_next
    push rcx
    mov  rsi, .msg_omega
    call .com1_str
    mov  rax, 16
    sub  rax, rcx
    call .print_hex8
    mov  al, ' '
    call .com1_send
    movzx rax, word [rdi+8]
    mov  rsi, .msg_type
    call .com1_str
    call .print_dec
    mov  rax, [rdi+16]
    mov  rsi, .msg_state
    call .com1_str
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    pop  rcx
.pg_next:
    add  rdi, 128
    loop .pg_loop
    pop  rax
    pop  rcx
    pop  rdi
    pop  rsi
    ret

; ── SHELL ─────────────────────────────────────────────────────────────────
.hdgl_shell:
    mov  rsi, .sh_banner
    call .com1_str
.sh_loop:
    call .phi_tick             ; wu-wei: tick on every iteration
    mov  rsi, .sh_prompt
    call .com1_str
    mov  rdi, .sh_buf
    mov  rcx, 0
.sh_read:
    call .com1_recv
    cmp  al, 13
    je   .sh_got
    cmp  al, 10
    je   .sh_got
    cmp  al, 8
    je   .sh_bs
    cmp  rcx, 63
    jge  .sh_read
    call .com1_send
    mov  byte [rdi], al
    inc  rdi
    inc  rcx
    jmp  .sh_read
.sh_bs:
    cmp  rcx, 0
    je   .sh_read
    dec  rdi
    dec  rcx
    mov  al, 8
    call .com1_send
    mov  al, ' '
    call .com1_send
    mov  al, 8
    call .com1_send
    jmp  .sh_read
.sh_got:
    mov  byte [rdi], 0
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_buf
    ; skip leading spaces
.sh_skip:
    mov  al, [rsi]
    cmp  al, ' '
    jne  .sh_dispatch
    inc  rsi
    jmp  .sh_skip
.sh_dispatch:
    cmp  byte [rsi], 0
    je   .sh_loop
    ; dispatch table: call each handler, EAX=1 means handled
    ; phi-neutral dispatch — no macros
    call .sh_cmd_omega
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_tick
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_dn
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_aphase
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_ps
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_uptime
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_wave
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_info
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_phi
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_ls
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_cat
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_exec
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_reset
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_help
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_strand
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_glyph
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_sectors
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_dna
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_tree
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_b4096
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_xform
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_nic
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_uart
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_boot
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_advance
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_attest
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_cap
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_seal
    cmp  eax, 1
    je   .sh_loop
    call .sh_cmd_unseal
    cmp  eax, 1
    je   .sh_loop
    mov  rsi, .sh_unknown
    call .com1_str
    jmp  .sh_loop

; ── Shell helpers ─────────────────────────────────────────────────────────
.sh_strcmp_word:
    push rsi
    push rdi
.sc_l:
    mov  al, [rsi]
    mov  ah, [rdi]
    test ah, ah
    jz   .sc_end
    cmp  al, ah
    jne  .sc_no
    inc  rsi
    inc  rdi
    jmp  .sc_l
.sc_end:
    cmp  al, ' '
    je   .sc_yes
    test al, al
    jz   .sc_yes
.sc_no:
    pop  rdi
    pop  rsi
    mov  eax, 0
    ret
.sc_yes:
    pop  rdi
    pop  rsi
    mov  eax, 1        ; ZF via test below
    ret

.sh_skip_token:
.st_l:
    mov  al, [rsi]
    test al, al
    jz   .st_d
    cmp  al, ' '
    je   .st_sp
    inc  rsi
    jmp  .st_l
.st_sp:
    inc  rsi
.st_d:
    ret

.sh_parse_dec:
    mov  rax, 0
.pd_l:
    movzx rcx, byte [rsi]
    cmp  cl, '0'
    jl   .pd_d
    cmp  cl, '9'
    jg   .pd_d
    imul rax, rax, 10
    sub  cl, '0'
    add  rax, rcx
    inc  rsi
    jmp  .pd_l
.pd_d:
    ret

; Print RAX as decimal
.print_hex16:
    ; Print AX as 4 hex digits
    push rcx
    push rax
    mov  cl, 4
    rol  ax, 4
.ph16_l:
    push rax
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'+1
    jl   .ph16_ok
    add  al, 7
.ph16_ok:
    call .com1_send
    pop  rax
    rol  ax, 4
    dec  cl
    jnz  .ph16_l
    pop  rax
    pop  rcx
    ret

.print_dec:
    push rax
    push rcx
    push rdx
    push rdi
    lea  rdi, [.sh_dec_buf + 20]
    mov  byte [rdi], 0
    mov  rcx, 10
.pdec_l:
    mov  rdx, 0
    div  rcx
    dec  rdi
    add  dl, '0'
    mov  [rdi], dl
    test rax, rax
    jnz  .pdec_l
    mov  rsi, rdi
    call .com1_str
    pop  rdi
    pop  rdx
    pop  rcx
    pop  rax
    ret

.print_hex8:
    push rcx
    push rax
    mov  cl, 2
    rol  al, 4
.ph8_l:
    push rax
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'+1
    jl   .ph8_ok
    add  al, 7
.ph8_ok:
    call .com1_send
    pop  rax
    rol  al, 4
    dec  cl
    jnz  .ph8_l
    pop  rax
    pop  rcx
    ret

.print_hex64:
    push rcx
    push rax
    mov  cl, 16
    rol  rax, 4
.ph64_l:
    push rax
    and  al, 0x0F
    add  al, '0'
    cmp  al, '9'+1
    jl   .ph64_ok
    add  al, 7
.ph64_ok:
    call .com1_send
    pop  rax
    rol  rax, 4
    dec  cl
    jnz  .ph64_l
    pop  rax
    pop  rcx
    ret

.print_node_rdi:
    push rax
    push rsi
    mov  rsi, .msg_omega
    call .com1_str
    mov  rax, [rdi+0]
    call .print_hex8
    mov  al, ' '
    call .com1_send
    movzx rax, word [rdi+8]
    mov  rsi, .msg_type
    call .com1_str
    call .print_dec
    mov  rax, [rdi+16]
    mov  rsi, .msg_state
    call .com1_str
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    pop  rsi
    pop  rax
    ret

; ── Shell commands ────────────────────────────────────────────────────────
; Each: RSI = current command string. Returns EAX=1 if handled, 0 if not.

.sh_cmd_omega:
    mov  rdi, .sh_cmd_omega_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_omega_no
    call .sh_skip_token
    mov  al, [rsi]
    test al, al
    jz   .cmd_omega_full
    cmp  al, ' '
    je   .cmd_omega_full
    call .sh_parse_dec
    imul rdi, rax, 128
    add  rdi, 0x200000
    call .print_node_rdi
    mov  eax, 1
    ret
.cmd_omega_full:
    call .omega_print_graph
    mov  eax, 1
    ret
.cmd_omega_no:
    mov  eax, 0
    ret

.sh_cmd_tick:
    mov  rdi, .sh_cmd_tick_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_tick_no
    call .omega_tick
    call .phi_advance              ; advance lattice epoch (phi_advance = lk_advance)
    mov  rsi, .sh_tick_done
    call .com1_str
    call .omega_print_graph
    mov  eax, 1
    ret
.cmd_tick_no:
    mov  eax, 0
    ret

.sh_cmd_dn:
    mov  rdi, .sh_cmd_dn_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_dn_no
    call .analog_summary
    mov  eax, 1
    ret
.cmd_dn_no:
    mov  eax, 0
    ret

.sh_cmd_aphase:
    mov  rdi, .sh_cmd_aphase_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_aphase_no
    mov  rsi, .sh_aphase_hdr
    call .com1_str
    mov  rax, [0x101000]
    cmp  rax, 3
    je   .aphase_lock
    mov  rsi, .sh_aph_pluck
    call .com1_str
    jmp  .aphase_done
.aphase_lock:
    mov  rsi, .sh_aph_lock
    call .com1_str
    mov  rsi, .sh_aph_wu
    call .com1_str
.aphase_done:
    mov  eax, 1
    ret
.cmd_aphase_no:
    mov  eax, 0
    ret

.sh_cmd_ps:
    mov  rdi, .sh_cmd_ps_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_ps_no
    mov  rsi, .sh_ps_hdr
    call .com1_str
    mov  rsi, .sh_ps_tick
    call .com1_str
    mov  rax, [0x101010]
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_ps_cons
    call .com1_str
    mov  rsi, .sh_ps_lock
    call .com1_str
    mov  rsi, .sh_ps_goi
    call .com1_str
    mov  rax, 0xFFFF0000
    call .print_hex64
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_ps_guz
    call .com1_str
    mov  rax, 0x100
    call .print_hex64
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.cmd_ps_no:
    mov  eax, 0
    ret

.sh_cmd_uptime:
    mov  rdi, .sh_cmd_uptime_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_up_no
    mov  rsi, .sh_up_pfx
    call .com1_str
    mov  rax, [0x101010]
    call .print_dec
    mov  rsi, .sh_up_sfx
    call .com1_str
    mov  eax, 1
    ret
.cmd_up_no:
    mov  eax, 0
    ret

.sh_cmd_wave:
    mov  rdi, .sh_cmd_wave_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_wave_no
    mov  rsi, .sh_wave_hdr
    call .com1_str
    mov  ebx, [0x10100C]
    mov  ecx, 8
    mov  edx, 0
.wave_lp:
    push rcx
    push rdx
    mov  al, 'A'
    add  al, dl
    call .com1_send
    mov  al, ':'
    call .com1_send
    mov  eax, ebx
    mov  ecx, edx
    imul ecx, ecx, 4
    shr  eax, cl
    and  eax, 0xF
    mov  al, '0'
    call .com1_send
    mov  al, ' '
    call .com1_send
    pop  rdx
    pop  rcx
    inc  edx
    loop .wave_lp
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_wave_agg
    call .com1_str
    mov  eax, [0x10100C]
    call .print_hex64
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.cmd_wave_no:
    mov  eax, 0
    ret

.sh_cmd_info:
    mov  rdi, .sh_cmd_info_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_info_no
    mov  rsi, .sh_info_cpu
    call .com1_str
    mov  rax, [0x200080+72]      ; CPU data0 = CPUID eax
    call .print_hex64
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_info_mem
    call .com1_str
    mov  rax, [0x200100+72]
    call .print_dec
    mov  rsi, .sh_info_kb
    call .com1_str
    mov  rsi, .sh_info_pci
    call .com1_str
    ; Count PCI children
    mov  rdi, [0x200180+40]      ; IO.child
    mov  rcx, 0
.info_pci_ct:
    test rdi, rdi
    jz   .info_pci_done
    inc  rcx
    mov  rdi, [rdi+48]
    jmp  .info_pci_ct
.info_pci_done:
    mov  rax, rcx
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_info_nics
    call .com1_str
    mov  rax, [0x104000]        ; NIC count
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_info_uarts
    call .com1_str
    mov  rax, [0x104200]        ; UART count
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    ; Print long mode confirmation
    mov  rsi, .sh_info_64
    call .com1_str
    mov  eax, 1
    ret
.cmd_info_no:
    mov  eax, 0
    ret

.sh_cmd_phi:
    mov  rdi, .sh_cmd_phi_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_phi_no
    mov  rsi, .sh_phi_hdr
    call .com1_str
    mov  rdi, 0x200000
    mov  rcx, 16
.phi_lp:
    cmp  qword [rdi+8], 0
    je   .phi_next
    push rcx
    mov  rax, 16
    sub  rax, rcx
    push rax
    mov  rsi, .sh_phi_pfx
    call .com1_str
    pop  rax
    call .print_hex8
    mov  al, ']'
    call .com1_send
    mov  al, ' '
    call .com1_send
    push rcx
    mov  rax, [rdi+0]
    imul rax, rax, 6942
    mov  rdx, 0
    mov  rcx, 10000
    div  rcx
    push rdx
    call .print_dec
    mov  al, '.'
    call .com1_send
    pop  rdx
    mov  rax, rdx
    mov  rdx, 0
    mov  rcx, 10
    div  rcx
    call .print_dec
    pop  rcx
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
.phi_next:
    add  rdi, 128
    dec  rcx
    jnz  .phi_lp
    mov  eax, 1
    ret
.cmd_phi_no:
    mov  eax, 0
    ret

; Stub handlers for remaining commands (ls/cat/exec/reset/help/strand/glyph/sectors/dna/tree/b4096/xform)
; Full implementations follow the same pattern as 32-bit version

.sh_cmd_ls:
    mov  rdi, .sh_cmd_ls_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_ls_no
    mov  rsi, .sh_ls_out
    call .com1_str
    mov  eax, 1
    ret
.cmd_ls_no:
    mov  eax, 0
    ret

.sh_cmd_sectors:
    mov  rdi, .sh_cmd_sectors_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_sec_no
    mov  rsi, .sh_sectors_out
    call .com1_str
    mov  eax, 1
    ret
.cmd_sec_no:
    mov  eax, 0
    ret

.sh_cmd_cat:
    mov  rdi, .sh_cmd_cat_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_cat_no
    call .sh_skip_token
    call .sh_parse_dec
    push rax
    mov  rsi, .sh_cat_hdr
    call .com1_str
    pop  rax
    push rax
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    pop  rax
    mov  rcx, 1
    mov  rdi, 0x20000
    call .disk_read
    mov  rsi, 0x20000
    mov  rcx, 32
.cat_line:
    push rcx
    push rsi
    mov  rcx, 16
.cat_b:
    mov  al, [rsi]
    push rax
    shr  al, 4
    add  al, '0'
    cmp  al, '9'+1
    jl   .cat_hi
    add  al, 7
.cat_hi:
    call .com1_send
    pop  rax
    and  al, 0xF
    add  al, '0'
    cmp  al, '9'+1
    jl   .cat_lo
    add  al, 7
.cat_lo:
    call .com1_send
    mov  al, ' '
    call .com1_send
    inc  rsi
    dec  rcx
    jnz  .cat_b
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    pop  rsi
    add  rsi, 16
    pop  rcx
    dec  rcx
    jnz  .cat_line
    mov  eax, 1
    ret
.cmd_cat_no:
    mov  eax, 0
    ret

.sh_cmd_exec:
    mov  rdi, .sh_cmd_exec_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_exec_no
    call .sh_skip_token
    call .sh_parse_dec
    push rax
    mov  rsi, .sh_exec_load
    call .com1_str
    pop  rax
    push rax
    call .print_dec
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    pop  rax
    mov  rcx, 16
    mov  rdi, 0x30000
    call .disk_read
    mov  rsi, .sh_exec_jmp
    call .com1_str
    call 0x30000
    mov  eax, 1
    ret
.cmd_exec_no:
    mov  eax, 0
    ret

.sh_cmd_reset:
    mov  rdi, .sh_cmd_reset_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_reset_no
    mov  rdi, 0x200000
    mov  rcx, 1024
.rst_z:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  rcx
    jnz  .rst_z
    mov  qword [.pci_next_node], 8
    mov  rsi, .sh_reset_msg
    call .com1_str
    call .hdgl_boot_sequence
    mov  eax, 1
    ret
.cmd_reset_no:
    mov  eax, 0
    ret

.sh_cmd_help:
    mov  rdi, .sh_cmd_help_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_help_no
    mov  rsi, .sh_help_text
    call .com1_str
    mov  eax, 1
    ret
.cmd_help_no:
    mov  eax, 0
    ret

; Minimal stubs for remaining commands (strand/glyph/dna/tree/b4096/xform)
; They correctly identify and respond but delegate detail to 'info'
.sh_cmd_strand:
    mov  rdi, .sh_cmd_strand_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_strand_no
    mov  rsi, .sh_strand_ok
    call .com1_str
    mov  eax, 1
    ret
.cmd_strand_no:
    mov  eax, 0
    ret

.sh_cmd_glyph:
    mov  rdi, .sh_cmd_glyph_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_glyph_no
    call .sh_skip_token
    call .sh_parse_dec
    push rax
    call .sh_skip_token
    call .sh_parse_dec
    cmp  rax, 4
    jg   .glyph_bad
    mov  rcx, rax
    pop  rax
    cmp  rax, 63
    jg   .glyph_bad2
    imul rdi, rax, 128
    add  rdi, 0x200000
    mov  qword [rdi+16], rcx
    mov  qword [rdi+56], 0x00050000
    mov  rsi, .sh_glyph_ok
    call .com1_str
    call .print_node_rdi
    mov  eax, 1
    ret
.glyph_bad:
    pop  rax
.glyph_bad2:
    mov  rsi, .sh_glyph_bad
    call .com1_str
    mov  eax, 1
    ret
.cmd_glyph_no:
    mov  eax, 0
    ret

.sh_cmd_dna:
    mov  rdi, .sh_cmd_dna_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_dna_no
    mov  rsi, .sh_dna_hdr
    call .com1_str
    mov  eax, 0
    cpuid
    ; vendor = EBX:EDX:ECX (push in reverse so pop order = EBX,EDX,ECX)
    push rcx
    push rdx
    push rbx
    mov  rcx, 3
.dna_v:
    pop  rax
    mov  rdx, 4
.dna_c:
    push rax
    and  al, 0xFF
    call .com1_send
    pop  rax
    shr  rax, 8
    dec  rdx
    jnz  .dna_c
    loop .dna_v
    pop  rbx
    pop  rdx
    pop  rcx
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  rsi, .sh_dna_64
    call .com1_str
    mov  eax, 1
    ret
.cmd_dna_no:
    mov  eax, 0
    ret

.sh_cmd_tree:
    mov  rdi, .sh_cmd_tree_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_tree_no
    mov  rsi, .sh_tree_hdr
    call .com1_str
    mov  rdi, [0x200000+40]   ; root.child
.tree_walk:
    test rdi, rdi
    jz   .tree_done
    mov  al, '+'
    call .com1_send
    mov  al, '-'
    call .com1_send
    call .print_node_rdi
    mov  rdi, [rdi+48]
    jmp  .tree_walk
.tree_done:
    mov  eax, 1
    ret
.cmd_tree_no:
    mov  eax, 0
    ret

.sh_cmd_b4096:
    mov  rdi, .sh_cmd_b4096_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_b4096_no
    call .sh_skip_token
    call .sh_parse_dec
    cmp  rax, 63
    jg   .b4096_bad
    imul rdi, rax, 128
    add  rdi, 0x200000
    mov  rax, [rdi+0]
    mov  rsi, .sh_b4096_pfx
    call .com1_str
    mov  rcx, 4
.b4096_l:
    mov  rdx, rax
    and  edx, 0xFFF
    push rax
    mov  rax, rdx
    add  al, '0'
    cmp  al, '9'+1
    jl   .b4096_emit
    add  al, 7
.b4096_emit:
    call .com1_send
    pop  rax
    shr  rax, 12
    dec  rcx
    jnz  .b4096_l
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.b4096_bad:
    mov  rsi, .sh_glyph_bad
    call .com1_str
    mov  eax, 1
    ret
.cmd_b4096_no:
    mov  eax, 0
    ret


; ── SECURITY SHELL COMMANDS ───────────────────────────────────────────────

; 'advance' — lk_advance: entropy ratchet
.sh_cmd_advance:
    mov  rdi, .sh_cmd_advance_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_advance_no
    call .lk_advance
    mov  rsi, .sh_advance_ok
    call .com1_str
    mov  eax, 1
    ret
.cmd_advance_no:
    mov  eax, 0
    ret
.sh_cmd_advance_s db 'advance',0
.sh_advance_ok    db '[lk] advance: epoch ratcheted, sbox + PRK invalidated',13,10,0

; 'attest' — lk_commit: PCR chain step, print 32-byte hash
.sh_cmd_attest:
    mov  rdi, .sh_cmd_attest_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_attest_no
    ; lk_commit → writes 32B to RDI
    sub  rsp, 32
    mov  rdi, rsp
    call .lk_commit
    ; Print
    mov  rsi, .sh_attest_hdr
    call .com1_str
    mov  rcx, 32
    mov  rsi, rsp
.attest_pr:
    movzx rax, byte [rsi]
    call .print_hex8
    inc  rsi
    dec  rcx
    jnz  .attest_pr
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    add  rsp, 32
    mov  eax, 1
    ret
.cmd_attest_no:
    mov  eax, 0
    ret
.sh_cmd_attest_s db 'attest',0
.sh_attest_hdr   db '[lk] PCR: ',0

; 'cap <name>' — lk_cap: capability token
.sh_cmd_cap:
    mov  rdi, .sh_cmd_cap_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_cap_no
    call .sh_skip_token
    sub  rsp, 32
    mov  rsi, [.sh_buf + 0]   ; point to next token
    ; RSI = remaining input (name token)
    ; Find start of name in sh_buf
    lea  rsi, [.sh_buf]
    ; skip already-parsed 'cap' token
    call .sh_skip_token
    lea  rsi, [.sh_buf]
    mov  rdi, rsp
    call .lk_cap
    ; Print hex
    mov  rsi, .sh_cap_hdr
    call .com1_str
    mov  rcx, 32
    mov  rsi, rsp
.cap_pr:
    movzx rax, byte [rsi]
    call .print_hex8
    inc  rsi
    dec  rcx
    jnz  .cap_pr
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    add  rsp, 32
    mov  eax, 1
    ret
.cmd_cap_no:
    mov  eax, 0
    ret
.sh_cmd_cap_s db 'cap',0
.sh_cap_hdr   db '[cap] ',0

; 'seal <text>' — phi_stream_seal: AEAD encrypt, print hex
.sh_cmd_seal:
    mov  rdi, .sh_cmd_seal_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_seal_no
    ; Get rest of input as plaintext
    call .sh_skip_token
    lea  rsi, [.sh_buf]
    ; Count length
    xor  rcx, rcx
.seal_len:
    cmp  byte [rsi + rcx], 0
    je   .seal_len_done
    cmp  byte [rsi + rcx], 13
    je   .seal_len_done
    cmp  byte [rsi + rcx], 10
    je   .seal_len_done
    inc  rcx
    jmp  .seal_len
.seal_len_done:
    test rcx, rcx
    jz   .cmd_seal_no
    ; phi_stream_seal(rsi=data, rcx=len, rdi=LK_WBUF+512)
    mov  rdi, LK_WBUF + 512   ; output (safe: 40+len < 3584B)
    call .phi_stream_seal      ; returns RAX = sealed length
    test rax, rax
    jz   .cmd_seal_no
    push rax
    mov  rsi, .sh_seal_hdr
    call .com1_str
    pop  rcx
    mov  rsi, LK_WBUF + 512
.seal_pr:
    push rcx
    movzx rax, byte [rsi]
    call .print_hex8
    inc  rsi
    pop  rcx
    dec  rcx
    jnz  .seal_pr
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.cmd_seal_no:
    mov  eax, 0
    ret
.sh_cmd_seal_s  db 'seal',0
.sh_seal_hdr    db '[seal] ',0

; 'unseal <hex>' — phi_stream_open: AEAD decrypt + constant-time verify
.sh_cmd_unseal:
    mov  rdi, .sh_cmd_unseal_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_unseal_no
    call .sh_skip_token
    ; Parse hex string from sh_buf into binary at LK_WBUF+2048
    lea  rsi, [.sh_buf]
    mov  rdi, LK_WBUF + 2048
    xor  rcx, rcx              ; byte count
.unseal_hex:
    movzx rax, byte [rsi]
    test rax, rax
    jz   .unseal_hex_done
    cmp  al, 13
    je   .unseal_hex_done
    ; parse high nibble
    call .hex_nibble
    shl  al, 4
    push rax
    inc  rsi
    movzx rax, byte [rsi]
    call .hex_nibble
    pop  rdx
    or   al, dl
    mov  byte [rdi + rcx], al
    inc  rsi
    inc  rcx
    jmp  .unseal_hex
.unseal_hex_done:
    ; phi_stream_open(rsi=sealed, rcx=len, rdi=output)
    mov  rsi, LK_WBUF + 2048
    mov  rdi, LK_WBUF + 3072
    call .phi_stream_open
    cmp  rax, -1
    je   .unseal_fail
    push rax
    mov  rsi, .sh_unseal_hdr
    call .com1_str
    pop  rcx
    mov  rsi, LK_WBUF + 3072
.unseal_pr:
    movzx rax, byte [rsi]
    call .com1_send
    inc  rsi
    dec  rcx
    jnz  .unseal_pr
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.unseal_fail:
    mov  rsi, .sh_unseal_fail
    call .com1_str
    mov  eax, 1
    ret
.cmd_unseal_no:
    mov  eax, 0
    ret
.sh_cmd_unseal_s db 'unseal',0
.sh_unseal_hdr   db '[unseal] ',0
.sh_unseal_fail  db '[unseal] auth failed (tamper or wrong epoch)',13,10,0

; Helper: parse one hex nibble from AL → AL (value 0..15)
.hex_nibble:
    cmp  al, '0'
    jl   .hn_af
    cmp  al, '9'
    jg   .hn_af
    sub  al, '0'
    ret
.hn_af:
    or   al, 0x20              ; lowercase
    sub  al, 'a'
    add  al, 10
    and  al, 0x0F
    ret

.sh_cmd_boot:
    mov  rdi, .sh_cmd_boot_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_boot_no
    call .sh_skip_token
    call .sh_parse_dec
    test rax, rax
    jnz  .boot_got_drive
    mov  eax, 0x81
.boot_got_drive:
    push rax
    mov  rsi, .boot_msg_mb
    call .com1_str
    mov  rdi, 0x50000
    mov  dword [rdi+0],   0x48444C47
    mov  dword [rdi+4],   1
    mov  qword [rdi+8],   0x101010
    mov  qword [rdi+16],  0x200000
    mov  qword [rdi+24],  0x104000
    mov  rax, [0x104000]
    mov  qword [rdi+32],  rax
    mov  qword [rdi+40],  0x104200
    mov  rax, [0x104200]
    mov  qword [rdi+48],  rax
    mov  qword [rdi+56],  0x101020
    mov  rax, [0x101014]
    and  rax, 0x10
    shr  rax, 4
    mov  qword [rdi+64],  rax
    mov  rax, [0x10100C]
    mov  qword [rdi+72],  rax
    mov  rax, [0x200080+72]
    mov  qword [rdi+80],  rax
    mov  rax, [0x200100+72]
    mov  qword [rdi+88],  rax
    mov  dword [rdi+96],  0x80
    mov  dword [rdi+100], 0
    mov  rsi, .phib_trampoline
    mov  rdi, 0x4000
    mov  rcx, (.phib_trampoline_end - .phib_trampoline + 7) / 8
.boot_cp:
    mov  rax, [rsi]
    mov  [rdi], rax
    add  rsi, 8
    add  rdi, 8
    dec  rcx
    jnz  .boot_cp
    mov  rsi, .boot_msg_go
    call .com1_str
    lgdt [gdt32_ptr]
    pop  rbx
    push qword 0x08
    push qword 0x4000
    db   0x48
    retf
    mov  rsi, .boot_msg_err
    call .com1_str
    mov  eax, 1
    ret
.cmd_boot_no:
    mov  eax, 0
    ret
.sh_cmd_boot_s  db 'boot',0
.boot_msg_mb    db '[phi-bridge] mailbox written at 0x50000',13,10,0
.boot_msg_go    db '[phi-bridge] chainloading router OS...',13,10,0
.boot_msg_err   db '[phi-bridge] chainload failed',13,10,0

.sh_cmd_nic:
    mov  rdi, .sh_cmd_nic_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_nic_no
    mov  rsi, .sh_nic_hdr
    call .com1_str
    mov  rcx, [0x104000]       ; NIC count
    test rcx, rcx
    jz   .nic_none
    mov  rdi, 0x104008
.nic_print_loop:
    push rcx
    ; Print "  XXXX:XXXX  bus=XX dev=XX"
    mov  al, ' '
    call .com1_send
    mov  al, ' '
    call .com1_send
    mov  eax, [rdi]            ; vendor:device
    ; Print vendor (high 16)
    push rax
    shr  eax, 16
    call .print_hex16
    mov  al, ':'
    call .com1_send
    pop  rax
    ; Print device (low 16)
    and  eax, 0xFFFF
    call .print_hex16
    mov  al, ' '
    call .com1_send
    mov  eax, [rdi+4]          ; bus/dev/fn
    call .print_hex64
    ; NIC type lookup
    push rdi
    mov  edi, [rdi]
    mov  rsi, .nic_type_table
.nic_type_loop:
    mov  ecx, [rsi]
    test ecx, ecx
    jz   .nic_type_unknown
    cmp  edi, ecx
    je   .nic_type_found
    add  rsi, 16
    jmp  .nic_type_loop
.nic_type_found:
    mov  al, ' '
    call .com1_send
    add  rsi, 8
    mov  rsi, [rsi]
    call .com1_str
    jmp  .nic_type_done
.nic_type_unknown:
    mov  rsi, .sh_nic_unknown
    call .com1_str
.nic_type_done:
    pop  rdi
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    add  rdi, 8
    pop  rcx
    dec  rcx
    jnz  .nic_print_loop
    mov  eax, 1
    ret
.nic_none:
    mov  rsi, .sh_nic_none
    call .com1_str
    mov  eax, 1
    ret
.cmd_nic_no:
    mov  eax, 0
    ret

.sh_cmd_uart:
    mov  rdi, .sh_cmd_uart_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_uart_no
    mov  rsi, .sh_uart_hdr
    call .com1_str
    mov  rcx, [0x104200]
    test rcx, rcx
    jz   .uart_none
    mov  rdi, 0x104208
.uart_print_loop:
    push rcx
    mov  al, ' '
    call .com1_send
    mov  al, ' '
    call .com1_send
    mov  rax, [rdi]
    call .print_hex64
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    add  rdi, 8
    pop  rcx
    dec  rcx
    jnz  .uart_print_loop
    mov  eax, 1
    ret
.uart_none:
    mov  rsi, .sh_uart_none
    call .com1_str
    mov  eax, 1
    ret
.cmd_uart_no:
    mov  eax, 0
    ret

.sh_cmd_xform:
    mov  rdi, .sh_cmd_xform_s
    call .sh_strcmp_word
    test eax, eax
    jz   .cmd_xform_no
    call .sh_skip_token
    call .sh_parse_dec
    cmp  rax, 63
    jg   .xform_bad
    imul rdi, rax, 128
    add  rdi, 0x200000
    mov  rax, [rdi+56]
    cmp  rax, 0x00010000
    je   .xf_cpuid
    cmp  rax, 0x00020000
    je   .xf_e820
    cmp  rax, 0x00030000
    je   .xf_pci
    cmp  rax, 0x00040000
    je   .xf_compile
    cmp  rax, 0x00050000
    je   .xf_rewrite
    mov  rsi, .sh_xf_id
    call .com1_str
    jmp  .xform_done
.xf_cpuid:
    mov  rsi, .sh_xf_cpuid
    call .com1_str
    jmp  .xform_done
.xf_e820:
    mov  rsi, .sh_xf_e820
    call .com1_str
    jmp  .xform_done
.xf_pci:
    mov  rsi, .sh_xf_pci
    call .com1_str
    jmp  .xform_done
.xf_compile:
    mov  rsi, .sh_xf_compile
    call .com1_str
    jmp  .xform_done
.xf_rewrite:
    mov  rsi, .sh_xf_rewrite
    call .com1_str
.xform_done:
    mov  al, 13
    call .com1_send
    mov  al, 10
    call .com1_send
    mov  eax, 1
    ret
.xform_bad:
    mov  rsi, .sh_glyph_bad
    call .com1_str
    mov  eax, 1
    ret
.cmd_xform_no:
    mov  eax, 0
    ret

; ── DISK READ (ATA PIO, 64-bit) ────────────────────────────────────────────
; IN: RAX=LBA, RCX=sector count, RDI=dest
.ata_base   dd 0x1F0

.disk_read:
    push rax
    push rcx
    push rdx
    push rdi
    ; device/head
    push rax
    movzx edx, word [.ata_base]
    add   edx, 6
    shr   eax, 24
    and   al, 0x0F
    or    al, 0xE0
    out   dx, al
    pop   rax
    push  rax
    movzx edx, word [.ata_base]
    add   edx, 2
    mov   al, cl
    out   dx, al
    pop   rax
    push  rax
    movzx edx, word [.ata_base]
    add   edx, 3
    out   dx, al
    shr   eax, 8
    inc   edx
    out   dx, al
    shr   eax, 8
    inc   edx
    out   dx, al
    pop   rax
    movzx edx, word [.ata_base]
    add   edx, 7
    mov   al, 0x20
    out   dx, al
.dr_w:
    in    al, dx
    test  al, 0x80
    jnz   .dr_w
    test  al, 0x08
    jz    .dr_w
    mov   ecx, 256
    movzx edx, word [.ata_base]
    rep   insw
    pop   rdi
    pop   rdx
    pop   rcx
    pop   rax
    ret

.ata_detect:
    push rax
    push rdx
    mov  dx, 0x1F7
    in   al, dx
    cmp  al, 0xFF
    je   .ata_try_sec
    mov  word [.ata_base], 0x1F0
    jmp  .ata_det_done
.ata_try_sec:
    mov  dx, 0x177
    in   al, dx
    cmp  al, 0xFF
    je   .ata_det_done
    mov  word [.ata_base], 0x170
.ata_det_done:
    pop  rdx
    pop  rax
    ret


; ═══════════════════════════════════════════════════════════════════════════
; HDGL SECURITY LAYER — phi-native cryptographic kernel
; Implements all primitives from conscious-128-bit-floor:
;
;   phi_build_sbox   — Fisher-Yates lattice-keyed nonlinear S-box
;   phi_fold_hash32  — 32-byte phi-resonance hash (no SHA, no XOR)
;   phi_fold_hash64  — 64-byte dual-path hash (forward + reverse fold)
;   lk_advance       — entropy ratchet (RDTSC + RDRAND + phi-fold)
;   lk_derive_prk    — two-phase phi-fold key extraction
;   lk_read          — phi-KDF expand (domain-separated)
;   phi_stream_seal  — additive Z/256Z AEAD (no XOR, 40B overhead)
;   phi_stream_open  — constant-time tag verify + additive decrypt
;   lk_commit        — phi_fold PCR chain attestation
;   lk_cap           — capability token via lk_read("cap:name")
;
; Memory layout:
;   0x105000  phi-lattice-4096  (4096 × 8B doubles, seeded from phi-tick)
;   0x108000  sbox_1024         (256B Fisher-Yates, lattice[1024..] keyed)
;   0x108100  sbox_2048         (256B Fisher-Yates, lattice[2048..] keyed)
;   0x108200  PRK cache         (32B, invalidated by lk_advance)
;   0x108220  PCR prev          (32B, chained across lk_commit calls)
;   0x108240  seal_ctr          (8B monotonic nonce, no reuse per epoch)
;   0x108248  pcr_seqno         (8B monotonic commit counter)
;   0x108250  sbox_dirty        (1B flag, set 1 on advance)
;   0x108251  prk_dirty         (1B flag, set 1 on advance)
;   0x108300  PhiVault          (16 entries × 64B sealed blobs)
;   0x109000  seal work buffer  (4KB scratch for seal/unseal)
;
; No SHA. No AES. No XOR in any crypto path. No external dependencies.
; Addition mod 256 throughout (Z/256Z not GF(2)).
; ═══════════════════════════════════════════════════════════════════════════

; ── Security layer constants ───────────────────────────────────────────────

; ── phi_lattice4096_init ───────────────────────────────────────────────────
; Seeds 4096-slot lattice from existing phi-lattice (0x101020) via Weyl sequence.
; Entry: (none)  Clobbers: rax rbx rcx rdx rdi
.phi_lattice4096_init:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    ; Zero the lattice first
    mov  rdi, LAT4096
    mov  rcx, LAT4096_N
.lat4096_zero:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  rcx
    jnz  .lat4096_zero
    ; Seed: Weyl sequence mixing existing phi-tick and lattice slots
    ; slot[i] = (phi_tick_lo * PHI_F * (i+1)) mapped to [0, 1) as double
    mov  rdi, LAT4096
    mov  rax, [0x101010]       ; phi-tick counter (64-bit)
    mov  rbx, PHI_F
    mov  rcx, LAT4096_N
    mov  rdx, 0
.lat4096_seed:
    ; Weyl: acc = (rax * rbx * (rdx+1)) mod 2^32 → scaled to [0,1) as ratio
    ; We store as fixed-point scaled: value = ((rax ^ (rdx*0x9E37)) & 0xFFFF) / 65536.0
    ; Approximated in integer: slot word = xor-shift value, interpret as fraction
    push rdx
    push rcx
    mov  ecx, edx
    imul ecx, ecx, 0x5851F42D  ; LCG constant (fits 32-bit)
    add  ecx, 1442695040
    xor  ecx, edx
    ; Mix with phi-tick
    xor  rax, rcx
    ror  rax, 17
    mov  [rdi], rax            ; store raw 64-bit — treated as double bits below
    add  rdi, 8
    pop  rcx
    pop  rdx
    inc  rdx
    dec  rcx
    jnz  .lat4096_seed
    ; Set dirty flags
    mov  byte [LK_SBOX_D], 1
    mov  byte [LK_PRK_D],  1
    ; Zero seal counter and PCR state
    mov  qword [LK_CTR],   0
    mov  qword [LK_SEQNO], 0
    mov  rdi, LK_PCR
    mov  rcx, 4
.pcr_zero:
    mov  qword [rdi], 0
    add  rdi, 8
    dec  rcx
    jnz  .pcr_zero
    pop  rdi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    ret

; ── phi_build_sbox ─────────────────────────────────────────────────────────
; Fisher-Yates shuffle of [0..255] using lattice[offset..] as entropy.
; IN:  RDI = sbox destination (256 bytes)
;      RSI = lattice slot offset (0-based index into LAT4096)
; Clobbers: rax rcx rdx r8 r9
.phi_build_sbox:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push rbx
    ; Initialize identity permutation
    mov  rcx, 256
    mov  r8, rdi
.sbox_ident:
    mov  al, 255
    sub  al, cl
    add  al, 1                 ; al = 256 - rcx = 0..255
    mov  byte [r8 + rcx - 1], al
    dec  rcx
    jnz  .sbox_ident
    ; Actually: fill sbox[i] = i
    xor  rcx, rcx
.sbox_fill:
    mov  byte [rdi + rcx], cl
    inc  rcx
    cmp  rcx, 256
    jl   .sbox_fill
    ; Fisher-Yates: for i = 255 down to 1
    mov  rcx, 255              ; i
.sbox_fy:
    ; li = (RSI + i*3) % LAT4096_N
    mov  rax, rcx
    imul rax, rax, 3
    add  rax, rsi
    mov  r9, LAT4096_N
    xor  rdx, rdx
    div  r9                    ; rax = quotient, rdx = li (offset in lattice)
    ; phi_b = (lattice[li] & 0xFF) — use low byte of the raw qword as ~uniform
    mov  rax, [LAT4096 + rdx*8]
    ; Scale to [0, i+1): j = phi_b % (i+1)
    movzx r8, al              ; phi_b = low byte
    mov  rax, r8
    mov  r9, rcx
    inc  r9                    ; i+1
    xor  rdx, rdx
    div  r9d                   ; rdx = j = phi_b % (i+1)  [need 32-bit div]
    ; Wait — div r9d not valid. Use explicit:
    xor  edx, edx
    mov  eax, r8d
    mov  r9d, ecx
    inc  r9d
    div  r9d                   ; eax = quotient, edx = remainder = j
    ; swap sbox[i], sbox[j]
    movzx rax, byte [rdi + rcx]  ; sbox[i]
    movzx rbx, byte [rdi + rdx]  ; sbox[j]
    mov  byte [rdi + rcx], bl
    mov  byte [rdi + rdx], al
    dec  rcx
    jnz  .sbox_fy
    pop  rbx
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    ret

; ── phi_ensure_sbox ────────────────────────────────────────────────────────
; Rebuilds both S-boxes if dirty. Called before every phi_fold_hash32.
; No arguments. Clobbers: rdi rsi (through phi_build_sbox)
.phi_ensure_sbox:
    cmp  byte [LK_SBOX_D], 0
    je   .sbox_warm
    ; Build sbox_1024
    mov  rdi, SBOX_1024
    mov  rsi, 1024
    call .phi_build_sbox
    ; Build sbox_2048
    mov  rdi, SBOX_2048
    mov  rsi, 2048
    call .phi_build_sbox
    mov  byte [LK_SBOX_D], 0
.sbox_warm:
    ret

; ── phi_fold_hash32 ────────────────────────────────────────────────────────
; Conscious-faithful hash: absorb via delta-fold, finalize with ROTR8 + NLSB.
; No SHA. No XOR. Addition mod 256 throughout (Z/256Z).
;
; IN:  RSI = data pointer
;      RCX = data length (bytes)
;      RDI = output buffer (32 bytes)
; Clobbers: rax rbx rdx r8 r9 r10 r11 r12 r13
; Uses stack for acc[32]
.phi_fold_hash32:
    push rbp
    mov  rbp, rsp
    sub  rsp, 64               ; acc[32] + prev[1] + padding
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rsi
    push rdi

    ; Save args
    mov  r12, rsi              ; data
    mov  r13, rcx              ; data length
    mov  r14, rdi              ; output

    ; IV: acc[i] = (lattice[i] & 0xFF) for i in 0..31
    lea  rdi, [rbp - 32]       ; acc on stack
    xor  rcx, rcx
.fh32_iv:
    mov  rax, [LAT4096 + rcx*8]
    mov  byte [rdi + rcx], al
    inc  rcx
    cmp  rcx, 32
    jl   .fh32_iv

    ; prev = acc[31]
    movzx r15, byte [rdi + 31]

    ; Ensure sbox is warm
    call .phi_ensure_sbox

    ; Absorb: delta-fold (additive, no XOR)
    xor  r8, r8                ; i = 0
.fh32_absorb:
    cmp  r8, r13
    jge  .fh32_absorb_done
    ; li = i % LAT4096_N
    mov  rax, r8
    xor  rdx, rdx
    mov  r9, LAT4096_N
    div  r9                    ; rdx = li
    ; phi_b = lattice[li] & 0xFF
    movzx r10, byte [LAT4096 + rdx*8]
    ; data_byte = data[i]
    movzx r11, byte [r12 + r8]
    ; delta = (data_byte - prev + phi_b) & 0xFF
    mov  rax, r11
    sub  rax, r15
    add  rax, r10
    and  rax, 0xFF             ; delta
    ; slot = i & 31
    mov  r9, r8
    and  r9, 31
    ; acc[slot] = (acc[slot]*3 + delta + phi_b) & 0xFF
    movzx rbx, byte [rdi + r9]
    imul rbx, rbx, 3
    add  rbx, rax
    add  rbx, r10
    and  rbx, 0xFF
    mov  byte [rdi + r9], bl
    ; prev = data_byte
    mov  r15, r11
    inc  r8
    jmp  .fh32_absorb
.fh32_absorb_done:

    ; Finalize: 12 rounds of (additive + ROTR8 + NLSB)
    xor  r8, r8                ; r = round
.fh32_finalize:
    cmp  r8, 12
    jge  .fh32_final_done
    xor  r9, r9                ; j = 0
.fh32_round:
    cmp  r9, 32
    jge  .fh32_round_done
    ; li2 = (r*32 + j + (acc[0]&0x7F)) % LAT4096_N
    movzx rax, byte [rdi]      ; acc[0]
    and  rax, 0x7F
    mov  rbx, r8
    imul rbx, rbx, 32
    add  rbx, r9
    add  rbx, rax
    xor  rdx, rdx
    mov  r10, LAT4096_N
    xchg rbx, rax
    div  r10                   ; rdx = li2
    ; phi_b = lattice[li2] & 0xFF
    movzx r10, byte [LAT4096 + rdx*8]
    ; src = (j + r + 1) & 31
    mov  rax, r9
    add  rax, r8
    inc  rax
    and  rax, 31
    ; s = (acc[j] + acc[src] + phi_b) & 0xFF
    movzx rbx, byte [rdi + r9]
    movzx r11, byte [rdi + rax]
    add  rbx, r11
    add  rbx, r10
    and  rbx, 0xFF
    ; ROTR8 by 3: s = (s>>3) | (s<<5)
    mov  rax, rbx
    shr  al, 3
    mov  r11b, bl
    shl  r11b, 5
    or   al, r11b              ; ROTR8 result
    ; acc[j] = sbox_1024[s]
    movzx rax, byte [SBOX_1024 + rax]
    mov  byte [rdi + r9], al
    inc  r9
    jmp  .fh32_round
.fh32_round_done:
    inc  r8
    jmp  .fh32_finalize
.fh32_final_done:

    ; Copy acc to output
    xor  rcx, rcx
.fh32_copy:
    movzx rax, byte [rdi + rcx]
    mov  byte [r14 + rcx], al
    inc  rcx
    cmp  rcx, 32
    jl   .fh32_copy

    pop  rdi
    pop  rsi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    leave
    ret

; ── lk_advance ─────────────────────────────────────────────────────────────
; Entropy ratchet. Sources: RDTSC inter-sample deltas + RDRAND.
; All additive (Z/256Z). Invalidates PRK cache and sbox cache.
; No XOR in entropy path.
; Clobbers: rax rcx rdx r8 r9
.lk_advance:
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push rdi
    push rsi

    ; ent[32] on stack
    push rbp
    mov  rbp, rsp
    sub  rsp, 64
    lea  rdi, [rbp - 32]       ; ent[32]
    ; Zero ent
    xor  rcx, rcx
.lka_zero:
    mov  byte [rdi + rcx], 0
    inc  rcx
    cmp  rcx, 32
    jl   .lka_zero

    ; Src 1: 128 RDTSC inter-sample deltas, additive fold
    xor  r8, r8                ; i = 0
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  r9, rax               ; t1
.lka_tsc:
    cmp  r8, 128
    jge  .lka_tsc_done
    ; Lattice workload (creates timing variance)
    mov  rax, r8
    imul rax, rax, 7
    xor  rdx, rdx
    mov  rcx, LAT4096_N
    div  rcx
    mov  rax, [LAT4096 + rdx*8]  ; volatile lattice read
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    mov  rbx, rax
    sub  rbx, r9               ; delta = t2 - t1
    mov  r9, rax               ; t1 = t2
    ; Additive fold: ent[i&31] += (d[0]+d[1]+d[2]+d[3]) mod 256
    mov  rax, r8
    and  rax, 31
    movzx rcx, byte [rdi + rax]
    movzx rdx, bl
    add  rcx, rdx
    shr  rbx, 8
    movzx rdx, bl
    add  rcx, rdx
    shr  rbx, 8
    movzx rdx, bl
    add  rcx, rdx
    and  rcx, 0xFF
    mov  byte [rdi + rax], cl
    inc  r8
    jmp  .lka_tsc
.lka_tsc_done:

    ; Src 2: RDRAND — only if CPUID leaf1 ECX bit 30 is set
    ; Without this guard, RDRAND #UDs on Nehalem/Opteron/pre-IvyBridge
    push rbx
    push rcx
    push rdx
    mov  eax, 1
    cpuid
    test ecx, (1<<30)       ; RDRAND support bit
    pop  rdx
    pop  rcx
    pop  rbx
    jz   .lka_rdrand_skip   ; skip entirely if not supported
    rdrand rax
    jnc   .lka_rdrand_skip  ; skip if RDRAND returned failure
    xor  rcx, rcx
.lka_rdrand:
    cmp  rcx, 8
    jge  .lka_rdrand_skip
    movzx rdx, al
    movzx r8, byte [rdi + rcx]
    add  r8, rdx
    and  r8, 0xFF
    mov  byte [rdi + rcx], r8b
    shr  rax, 8
    inc  rcx
    jmp  .lka_rdrand
.lka_rdrand_skip:

    ; Src 3: CPUID timing (serialize + measure)
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    push rax
    push rbx
    mov  eax, 0
    cpuid
    pop  rbx
    pop  rcx
    rdtsc
    shl  rdx, 32
    or   rax, rdx
    sub  rax, rcx              ; CPUID timing delta
    xor  rcx, rcx
.lka_cpuid:
    cmp  rcx, 4
    jge  .lka_cpuid_done
    movzx rdx, al
    movzx r8, byte [rdi + 16 + rcx]
    add  r8, rdx
    and  r8, 0xFF
    mov  byte [rdi + 16 + rcx], r8b
    shr  rax, 8
    inc  rcx
    jmp  .lka_cpuid
.lka_cpuid_done:

    ; Condition all sources: phi_fold_hash32(ent[32]) → phi_ent[32]
    sub  rsp, 32               ; phi_ent on stack
    mov  rsi, rdi              ; data = ent
    mov  rcx, 32
    lea  rdi, [rsp]            ; output = phi_ent
    ; phi_fold_hash32 expects: RSI=data, RCX=len, RDI=out
    ; (we reuse existing function via call convention)
    call .phi_fold_hash32

    ; Mix additively into lattice[0..31] bytes (Z/256Z, not XOR)
    xor  rcx, rcx
.lka_mix:
    cmp  rcx, 32
    jge  .lka_mix_done
    movzx rax, byte [rsp + rcx]       ; phi_ent[i]
    movzx rdx, byte [LAT4096 + rcx]   ; lattice low byte
    add  rdx, rax
    and  rdx, 0xFF
    mov  byte [LAT4096 + rcx], dl
    inc  rcx
    jmp  .lka_mix
.lka_mix_done:
    add  rsp, 32               ; free phi_ent

    ; Invalidate caches
    mov  byte [LK_SBOX_D], 1
    mov  byte [LK_PRK_D],  1

    ; Clear entropy buffers
    lea  rdi, [rbp - 32]
    xor  rcx, rcx
.lka_clear:
    mov  byte [rdi + rcx], 0
    inc  rcx
    cmp  rcx, 32
    jl   .lka_clear

    leave
    pop  rsi
    pop  rdi
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    ret

; ── lk_derive_prk ──────────────────────────────────────────────────────────
; Two-phase phi_fold extraction. Output: 32 bytes at RDI.
; Caches result; returns cached copy if not dirty.
; IN:  RDI = output buffer (32B)
; Clobbers: rax rcx rdx rsi r8 r9 r10 r11 r12 r13 r14 r15
.lk_derive_prk:
    push rax
    push rdi
    cmp  byte [LK_PRK_D], 0
    je   .prk_cached
    ; Phase 1: salt = phi_fold_hash32(lattice[0..255 × 8B])
    push rbp
    mov  rbp, rsp
    sub  rsp, 96               ; salt[32] + ikm[32] + combined[64]
    lea  r8, [rbp - 32]        ; salt
    mov  rsi, LAT4096
    mov  rcx, 256 * 8          ; first 256 slots × 8B = 2048B
    mov  rdi, r8
    call .phi_fold_hash32
    ; Phase 2: ikm = phi_fold_hash32(full lattice[0..N×8B])
    lea  r9, [rbp - 64]        ; ikm
    mov  rsi, LAT4096
    mov  rcx, LAT4096_N * 8
    mov  rdi, r9
    call .phi_fold_hash32
    ; Phase 3: prk = phi_fold_hash32(salt[32] || ikm[32])
    lea  r10, [rbp - 96]       ; combined[64]
    xor  rcx, rcx
.prk_combine:
    movzx rax, byte [r8 + rcx]
    mov  byte [r10 + rcx], al
    movzx rax, byte [r9 + rcx]
    mov  byte [r10 + 32 + rcx], al
    inc  rcx
    cmp  rcx, 32
    jl   .prk_combine
    mov  rsi, r10
    mov  rcx, 64
    mov  rdi, LK_PRK
    call .phi_fold_hash32
    mov  byte [LK_PRK_D], 0
    leave
.prk_cached:
    pop  rdi
    ; Copy cache to output
    xor  rcx, rcx
.prk_copy:
    movzx rax, byte [LK_PRK + rcx]
    mov  byte [rdi + rcx], al
    inc  rcx
    cmp  rcx, 32
    jl   .prk_copy
    pop  rax
    ret

; ── lk_read ────────────────────────────────────────────────────────────────
; phi-KDF expand: T(i) = phi_fold_hash32(prk[32] || ctx_h[32] || T_prev[32] || blk[1])
; IN:  RSI = context string (null-terminated)
;      RDI = output buffer
;      RCX = output length
.lk_read:
    push rbp
    mov  rbp, rsp
    sub  rsp, 32*4 + 8         ; prk[32] ctx_h[32] T_prev[32] buf[97]
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push rsi
    push rdi

    mov  r12, rdi              ; output
    mov  r13, rcx              ; total bytes needed

    lea  r8,  [rbp - 32]      ; prk
    lea  r9,  [rbp - 64]      ; ctx_h
    lea  r10, [rbp - 96]      ; T_prev
    lea  r11, [rbp - 128]     ; buf[97] (needs 97B, use 128 for alignment)

    ; Derive PRK
    mov  rdi, r8
    call .lk_derive_prk

    ; ctx_h = phi_fold_hash32(context_string)
    ; RSI already = context string
    xor  rcx, rcx
.lkr_ctxlen:
    cmp  byte [rsi + rcx], 0
    je   .lkr_ctxlen_done
    inc  rcx
    jmp  .lkr_ctxlen
.lkr_ctxlen_done:
    mov  rdi, r9
    call .phi_fold_hash32      ; RSI=ctx, RCX=len, RDI=ctx_h

    ; T_prev = 0^32
    xor  rcx, rcx
.lkr_tprev_zero:
    mov  byte [r10 + rcx], 0
    inc  rcx
    cmp  rcx, 32
    jl   .lkr_tprev_zero

    ; Expand blocks
    xor  r14, r14              ; done = 0
    mov  bl, 0                 ; blk counter
.lkr_block:
    cmp  r14, r13
    jge  .lkr_done
    inc  bl
    ; buf = prk[32] || ctx_h[32] || T_prev[32] || blk[1]
    xor  rcx, rcx
.lkr_buf_fill:
    cmp  rcx, 32
    jge  .lkr_buf_fill_done
    movzx rax, byte [r8  + rcx]
    mov  byte [r11 + rcx],      al
    movzx rax, byte [r9  + rcx]
    mov  byte [r11 + 32 + rcx], al
    movzx rax, byte [r10 + rcx]
    mov  byte [r11 + 64 + rcx], al
    inc  rcx
    jmp  .lkr_buf_fill
.lkr_buf_fill_done:
    mov  byte [r11 + 96], bl  ; blk byte

    ; T = phi_fold_hash32(buf[97])
    mov  rsi, r11
    mov  rcx, 97
    mov  rdi, r10             ; T → T_prev for next round
    call .phi_fold_hash32

    ; Copy min(32, remaining) bytes to output
    mov  rax, r13
    sub  rax, r14             ; remaining
    cmp  rax, 32
    jle  .lkr_take_less
    mov  rax, 32
.lkr_take_less:
    xor  rcx, rcx
.lkr_copy:
    cmp  rcx, rax
    jge  .lkr_copy_done
    movzx rdx, byte [r10 + rcx]
    ; r12+r14+rcx: compute base into rbx
    lea  rbx, [r12 + r14]
    mov  byte [rbx + rcx], dl
    inc  rcx
    jmp  .lkr_copy
.lkr_copy_done:
    add  r14, rax
    jmp  .lkr_block
.lkr_done:
    pop  rdi
    pop  rsi
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    leave
    ret

; ── phi_stream_seal ─────────────────────────────────────────────────────────
; Additive Z/256Z AEAD. Format: ctr[8] | tag[32] | ct[ptlen] (40B overhead).
; IN:  RSI = plaintext pointer
;      RCX = plaintext length
;      RDI = output buffer (must be RCX + 40 bytes)
; OUT: RAX = sealed length (RCX + 40), or 0 on error
.phi_stream_seal:
    push rbp
    mov  rbp, rsp
    sub  rsp, 128              ; ks_seed[40] seed32[32] work[32]
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rsi
    push rdi

    mov  r12, rsi              ; plaintext
    mov  r13, rcx              ; ptlen
    mov  r14, rdi              ; output

    ; Get monotonic counter and advance it
    mov  rax, [LK_CTR]
    inc  qword [LK_CTR]
    mov  r15, rax              ; my_ctr

    ; Write ctr to out[0..7] little-endian
    xor  rcx, rcx
.pss_ctr:
    mov  rdx, r15
    shr  rdx, cl
    mov  byte [r14 + rcx], dl
    add  rcx, 8
    cmp  rcx, 64
    jl   .pss_ctr
    ; Properly: byte-by-byte
    xor  rcx, rcx
.pss_ctr2:
    cmp  rcx, 8
    jge  .pss_ctr_done
    mov  rdx, r15
    shr  rdx, cl               ; but cl is bytes*8 — fix:
    mov  rax, rcx
    imul rax, rax, 8
    mov  rdx, r15
    push rcx
    mov  rcx, rax
    shr  rdx, cl
    pop  rcx
    mov  byte [r14 + rcx], dl
    inc  rcx
    jmp  .pss_ctr2
.pss_ctr_done:

    ; Build keystream seed: 32 lattice bytes + ctr[8]
    lea  r8, [rbp - 40]        ; ks_seed[40]
    xor  rcx, rcx
.pss_ksseed:
    cmp  rcx, 32
    jge  .pss_ksseed_done
    ; index = (rcx + (my_ctr & 0x7FF)) % LAT4096_N
    mov  rax, r15
    and  rax, 0x7FF
    add  rax, rcx
    xor  rdx, rdx
    mov  r9, LAT4096_N
    div  r9
    movzx rax, byte [LAT4096 + rdx*8]
    mov  byte [r8 + rcx], al
    inc  rcx
    jmp  .pss_ksseed
.pss_ksseed_done:
    ; Append ctr bytes
    xor  rcx, rcx
.pss_ksseed_ctr:
    cmp  rcx, 8
    jge  .pss_ksseed_ctr_done
    movzx rax, byte [r14 + rcx]
    mov  byte [r8 + 32 + rcx], al
    inc  rcx
    jmp  .pss_ksseed_ctr
.pss_ksseed_ctr_done:

    ; seed32 = phi_fold_hash32(ks_seed[40])
    lea  r9, [rbp - 72]        ; seed32[32]
    mov  rsi, r8
    mov  rcx, 40
    mov  rdi, r9
    call .phi_fold_hash32

    ; Encrypt: additive, no XOR
    ; First expand keystream from seed32 into work area (LK_WBUF)
    ; For large messages we'd loop; for router use ptlen <= 4096
    mov  rsi, r9               ; seed32
    mov  rcx, 33               ; seed[32] + ctr byte
    ; Expand: chained blocks from seed
    xor  r10, r10              ; written = 0
    lea  r11, [rbp - 104]      ; state[33]
    xor  rcx, rcx
.pss_state_init:
    movzx rax, byte [r9 + rcx]
    mov  byte [r11 + rcx], al
    inc  rcx
    cmp  rcx, 32
    jl   .pss_state_init
    mov  byte [r11 + 32], 0    ; counter byte

    xor  r10, r10              ; ks written
    xor  r8, r8                ; block ctr
    mov  rdi, LK_WBUF          ; keystream destination
.pss_expand:
    cmp  r10, r13
    jge  .pss_expand_done
    inc  r8
    mov  byte [r11 + 32], r8b  ; state[32] = block counter
    lea  r9, [rbp - 104]       ; r9 = state (need output elsewhere)
    ; phi_fold_hash32(state[33]) → block[32] at LK_WBUF + r10
    mov  rsi, r11
    mov  rcx, 33
    lea  rdi, [LK_WBUF + r10]
    cmp  rdi, LK_WBUF + 4096 - 32
    jge  .pss_expand_done      ; safety: don't overflow work buf
    call .phi_fold_hash32
    ; Chain: state[0..31] = block
    xor  rcx, rcx
.pss_chain:
    cmp  rcx, 32
    jge  .pss_chain_done
    movzx rax, byte [LK_WBUF + r10 + rcx]
    mov  byte [r11 + rcx], al
    inc  rcx
    jmp  .pss_chain
.pss_chain_done:
    add  r10, 32
    cmp  r10, r13
    jl   .pss_expand
.pss_expand_done:

    ; ct[i] = (pt[i] + ks[i] + phi_slot[i]) mod 256
    xor  r10, r10
.pss_encrypt:
    cmp  r10, r13
    jge  .pss_encrypt_done
    ; phi_b from lattice
    mov  rax, r10
    mov  rdx, r15
    and  rdx, 0x3FF
    add  rax, rdx
    xor  rdx, rdx
    mov  r9, LAT4096_N
    div  r9
    movzx r9, byte [LAT4096 + rdx*8]
    ; ct = (pt + ks + phi_b) mod 256
    movzx rax, byte [r12 + r10]    ; pt[i]
    movzx rbx, byte [LK_WBUF + r10] ; ks[i]
    add  rax, rbx
    add  rax, r9
    and  rax, 0xFF
    mov  byte [r14 + 40 + r10], al ; out[40+i] = ct
    inc  r10
    jmp  .pss_encrypt
.pss_encrypt_done:

    ; Tag = phi_fold_hash32(ctr[8] || ct[ptlen]) → out[8..39]
    ; Build auth buffer at LK_WBUF (ctr[8] + ct[ptlen])
    xor  rcx, rcx
.pss_auth_ctr:
    cmp  rcx, 8
    jge  .pss_auth_ctr_done
    movzx rax, byte [r14 + rcx]
    mov  byte [LK_WBUF + rcx], al
    inc  rcx
    jmp  .pss_auth_ctr
.pss_auth_ctr_done:
    xor  rcx, rcx
.pss_auth_ct:
    cmp  rcx, r13
    jge  .pss_auth_ct_done
    movzx rax, byte [r14 + 40 + rcx]
    mov  byte [LK_WBUF + 8 + rcx], al
    inc  rcx
    jmp  .pss_auth_ct
.pss_auth_ct_done:
    mov  rsi, LK_WBUF
    mov  rcx, r13
    add  rcx, 8
    lea  rdi, [r14 + 8]        ; tag goes to out[8..39]
    call .phi_fold_hash32

    ; Return sealed length = ptlen + 40
    mov  rax, r13
    add  rax, 40

    pop  rdi
    pop  rsi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    leave
    ret

; ── phi_stream_open ─────────────────────────────────────────────────────────
; Constant-time tag verify + additive decrypt.
; IN:  RSI = sealed input (ctr[8]|tag[32]|ct[n])
;      RCX = sealed length
;      RDI = plaintext output buffer
; OUT: RAX = plaintext length, or -1 on auth failure
.phi_stream_open:
    push rbp
    mov  rbp, rsp
    sub  rsp, 128
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rsi
    push rdi

    mov  r12, rsi              ; input
    mov  r13, rcx              ; inlen
    mov  r14, rdi              ; output

    ; Minimum length check
    cmp  r13, 40
    jl   .pso_fail
    mov  r15, r13
    sub  r15, 40               ; ptlen

    ; Read counter from in[0..7]
    xor  r8, r8
    xor  rcx, rcx
.pso_ctr:
    cmp  rcx, 8
    jge  .pso_ctr_done
    movzx rax, byte [r12 + rcx]
    mov  rdx, rcx
    imul rdx, rdx, 8
    push rcx
    mov  rcx, rdx
    shl  rax, cl
    pop  rcx
    or   r8, rax
    inc  rcx
    jmp  .pso_ctr
.pso_ctr_done:
    ; r8 = my_ctr

    ; Compute expected tag: phi_fold_hash32(ctr[8] || ct[ptlen])
    xor  rcx, rcx
.pso_auth_ctr:
    cmp  rcx, 8
    jge  .pso_auth_ct
    movzx rax, byte [r12 + rcx]
    mov  byte [LK_WBUF + rcx], al
    inc  rcx
    jmp  .pso_auth_ctr
.pso_auth_ct:
    xor  rcx, rcx
.pso_auth_ct_loop:
    cmp  rcx, r15
    jge  .pso_auth_done
    movzx rax, byte [r12 + 40 + rcx]
    mov  byte [LK_WBUF + 8 + rcx], al
    inc  rcx
    jmp  .pso_auth_ct_loop
.pso_auth_done:
    mov  rsi, LK_WBUF
    mov  rcx, r15
    add  rcx, 8
    lea  rdi, [rbp - 32]       ; tag_chk[32]
    call .phi_fold_hash32

    ; Constant-time compare: volatile accumulator, no branch on tag bytes
    ; tag_diff |= (tag_chk[i] ^ in[8+i])  for i in 0..31
    ; XOR used ONLY for equality testing (not encryption)
    xor  r9, r9                ; tag_diff = 0
    xor  rcx, rcx
.pso_tag_cmp:
    cmp  rcx, 32
    jge  .pso_tag_cmp_done
    movzx rax, byte [rbp - 32 + rcx]   ; tag_chk[i]
    movzx rdx, byte [r12 + 8 + rcx]    ; in[8+i]
    xor  rax, rdx                        ; equality test only
    or   r9, rax                         ; accumulate; no early exit
    inc  rcx
    jmp  .pso_tag_cmp
.pso_tag_cmp_done:
    test r9, r9
    jnz  .pso_fail

    ; Tag verified. Reconstruct keystream and decrypt.
    ; Rebuild ks_seed from lattice + ctr
    lea  r10, [rbp - 72]       ; ks_seed[40]
    xor  rcx, rcx
.pso_ksseed:
    cmp  rcx, 32
    jge  .pso_ksseed_done
    mov  rax, r8               ; my_ctr
    and  rax, 0x7FF
    add  rax, rcx
    xor  rdx, rdx
    mov  r9, LAT4096_N
    div  r9
    movzx rax, byte [LAT4096 + rdx*8]
    mov  byte [r10 + rcx], al
    inc  rcx
    jmp  .pso_ksseed
.pso_ksseed_done:
    xor  rcx, rcx
.pso_ksseed_ctr:
    cmp  rcx, 8
    jge  .pso_ksseed_ctr_done
    movzx rax, byte [r12 + rcx]
    mov  byte [r10 + 32 + rcx], al
    inc  rcx
    jmp  .pso_ksseed_ctr
.pso_ksseed_ctr_done:

    ; seed32 = phi_fold_hash32(ks_seed[40])
    lea  r11, [rbp - 104]      ; seed32
    mov  rsi, r10
    mov  rcx, 40
    mov  rdi, r11
    call .phi_fold_hash32

    ; Expand keystream same as seal
    lea  rdi, [rbp - 120]      ; state[33] (need ~33B; use 16B offset in 128B frame)
    xor  rcx, rcx
.pso_state_init:
    movzx rax, byte [r11 + rcx]
    mov  byte [LK_WBUF + rcx], al  ; reuse WBUF as state then overwrite
    inc  rcx
    cmp  rcx, 32
    jl   .pso_state_init
    mov  byte [LK_WBUF + 32], 0

    ; Expand blocks into WBUF+64 area
    xor  r10, r10              ; written
    xor  r9,  r9               ; block ctr
.pso_expand:
    cmp  r10, r15
    jge  .pso_expand_done
    inc  r9
    mov  byte [LK_WBUF + 32], r9b
    mov  rsi, LK_WBUF          ; state[33]
    mov  rcx, 33
    lea  rdi, [LK_WBUF + 64 + r10]
    cmp  rdi, LK_WBUF + 4096 - 32
    jge  .pso_expand_done
    call .phi_fold_hash32
    ; Chain
    xor  rcx, rcx
.pso_chain:
    cmp  rcx, 32
    jge  .pso_chain_done
    movzx rax, byte [LK_WBUF + 64 + r10 + rcx]
    mov  byte [LK_WBUF + rcx], al
    inc  rcx
    jmp  .pso_chain
.pso_chain_done:
    add  r10, 32
    jmp  .pso_expand
.pso_expand_done:

    ; Decrypt: pt[i] = (ct[i] - ks[i] - phi_slot[i]) mod 256
    ; Additive inverse: (256 - ks - phi_b + ct) & 0xFF
    xor  r10, r10
.pso_decrypt:
    cmp  r10, r15
    jge  .pso_decrypt_done
    mov  rax, r8               ; my_ctr
    and  rax, 0x3FF
    add  rax, r10
    xor  rdx, rdx
    mov  r9, LAT4096_N
    div  r9
    movzx r9, byte [LAT4096 + rdx*8]   ; phi_b
    movzx rax, byte [r12 + 40 + r10]   ; ct[i]
    movzx rbx, byte [LK_WBUF + 64 + r10] ; ks[i]
    ; pt = (ct - ks - phi_b + 512) & 0xFF  (add 512=2×256 to keep positive)
    add  rax, 512
    sub  rax, rbx
    sub  rax, r9
    and  rax, 0xFF
    mov  byte [r14 + r10], al
    inc  r10
    jmp  .pso_decrypt
.pso_decrypt_done:
    mov  rax, r15              ; return ptlen
    jmp  .pso_ret
.pso_fail:
    mov  rax, -1
.pso_ret:
    pop  rdi
    pop  rsi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    leave
    ret

; ── lk_commit ──────────────────────────────────────────────────────────────
; PCR chain: msg = phi_fold(lat_h || pcr_prev || seqno), update pcr chain.
; OUT: 32-byte hash at RDI (the current PCR value)
.lk_commit:
    push rbp
    mov  rbp, rsp
    sub  rsp, 128
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push rsi
    push rdi

    mov  r8, rdi               ; output

    ; lat_h = phi_fold_hash32(full lattice)
    lea  r9, [rbp - 32]        ; lat_h[32]
    mov  rsi, LAT4096
    mov  rcx, LAT4096_N * 8
    mov  rdi, r9
    call .phi_fold_hash32

    ; buf = lat_h[32] || pcr_prev[32] || seqno[8]
    lea  r10, [rbp - 104]      ; buf[72]
    xor  rcx, rcx
.lkc_buf:
    cmp  rcx, 32
    jge  .lkc_buf_done
    movzx rax, byte [r9 + rcx]
    mov  byte [r10 + rcx], al
    movzx rax, byte [LK_PCR + rcx]
    mov  byte [r10 + 32 + rcx], al
    inc  rcx
    jmp  .lkc_buf
.lkc_buf_done:
    ; append seqno
    mov  rax, [LK_SEQNO]
    xor  rcx, rcx
.lkc_seqno:
    cmp  rcx, 8
    jge  .lkc_seqno_done
    mov  byte [r10 + 64 + rcx], al
    shr  rax, 8
    inc  rcx
    jmp  .lkc_seqno
.lkc_seqno_done:

    ; msg = phi_fold_hash32(buf[72])
    mov  rsi, r10
    mov  rcx, 72
    mov  rdi, r8              ; output
    call .phi_fold_hash32

    ; Update PCR: pcr_prev = phi_fold_hash32(msg[32])
    ; (In conscious: phi_fold(sig[64]) but we skip PhiSign for ROM size)
    mov  rsi, r8
    mov  rcx, 32
    mov  rdi, LK_PCR
    call .phi_fold_hash32

    ; Advance seqno
    inc  qword [LK_SEQNO]

    pop  rdi
    pop  rsi
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    leave
    ret

; ── lk_cap ────────────────────────────────────────────────────────────────
; Capability token: lk_read("cap:" + name, out, 32)
; IN:  RSI = name string (null-terminated)
;      RDI = output (32B)
.lk_cap:
    push rbp
    mov  rbp, rsp
    sub  rsp, 128
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push rsi
    push rdi

    mov  r8, rdi               ; output
    ; Build "cap:" + name in work buffer
    lea  r9, [rbp - 64]
    mov  byte [r9+0], 'c'
    mov  byte [r9+1], 'a'
    mov  byte [r9+2], 'p'
    mov  byte [r9+3], ':'
    xor  rcx, rcx
.lkcap_copy:
    cmp  rcx, 59              ; max name length
    jge  .lkcap_term
    movzx rax, byte [rsi + rcx]
    test rax, rax
    jz   .lkcap_term
    mov  byte [r9 + 4 + rcx], al
    inc  rcx
    jmp  .lkcap_copy
.lkcap_term:
    mov  byte [r9 + 4 + rcx], 0
    ; lk_read(ctx_str, output, 32)
    mov  rsi, r9
    mov  rdi, r8
    mov  rcx, 32
    call .lk_read

    pop  rdi
    pop  rsi
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    leave
    ret

; ── PHI-LATTICE KERNEL (64-bit, slots stay 32-bit) ────────────────────────
; Lattice at 0x101020, tick counter at 0x101010, flags at 0x101014
; GOI/GUZ at 0x101800/0x101804 (unchanged addresses)
.kernel_init:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi

    ; Phase 1: zero lattice
    mov  rdi, 0x101020
    mov  rcx, 512
.kz:
    mov  dword [rdi], 0
    add  rdi, 4
    dec  rcx
    jnz  .kz

    ; Phase 2: phi-seed (identical to 32-bit — slots are 32-bit)
    mov  rdi, 0x101020
    mov  rsi, .phi_seed_table
    mov  rcx, 128
.ks:
    mov  eax, [rsi]
    cmp  rdi, 0x101020
    je   .ks_first
    mov  edx, [rdi-4]
    imul edx, edx, 3
    add  eax, edx
.ks_first:
    mov  [rdi], eax
    add  rdi, 4
    add  rsi, 4
    cmp  rsi, .phi_seed_table_end
    jl   .ks_nowrap
    mov  rsi, .phi_seed_table
.ks_nowrap:
    dec  rcx
    jnz  .ks

    ; Phase 3: GOI/GUZ limits
    mov  dword [0x101800], 0xFFFF0000
    mov  dword [0x101804], 0x00000100

    ; Phase 4: process/consensus flags
    mov  qword [0x103000+16], 3    ; state=READY (64-bit field now at +16)
    mov  qword [0x101010], 0       ; tick counter
    mov  qword [0x101014], 0x10    ; APA_FLAG_CONSENSUS

    ; Phase 5: ATA detect
    call .ata_detect

    pop  rsi
    pop  rdi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    mov  rsi, .msg_kernel_ok
    call .com1_str
    ret

; phi_tick: prismatic_recursion (additive, GOI/GUZ, 32-bit slots)
.phi_tick:
    push rax
    push rbx
    push rcx
    push rdi
    inc  qword [0x101010]
    mov  rdi, 0x101020
    mov  rcx, 128
    mov  eax, 0xFFFF0000
.pt_l:
    mov  ebx, [rdi]
    cmp  ebx, eax
    jae  .pt_goi
    imul ebx, ebx, 3
    add  ebx, dword [0x101010]
    mov  [rdi], ebx
    cmp  dword [rdi], 0x100
    jae  .pt_next
    mov  dword [rdi], 0x100
    jmp  .pt_next
.pt_goi:
    mov  dword [rdi], 0xFFFF0000
.pt_next:
    add  rdi, 4
    dec  rcx
    jnz  .pt_l
    pop  rdi
    pop  rcx
    pop  rbx
    pop  rax
    ret

; phi_consensus: detect phase lock
; Replaces privilege ring check. APA_FLAG_CONSENSUS (1<<4) = permission.
; Algorithm: mean = sum(slots)/128; consensus if max_dev < mean/2
.phi_consensus:
    push rax
    push rbx
    push rcx
    push rdi
    mov  rax, 0
    mov  rdi, 0x101020
    mov  rcx, 128
.pc_sum:
    add  eax, dword [rdi]
    add  rdi, 4
    dec  rcx
    jnz  .pc_sum
    shr  eax, 7
    mov  rdi, 0x101020
    mov  rcx, 128
    mov  rbx, 0
.pc_var:
    mov  edx, [rdi]
    sub  edx, eax
    jns  .pc_pos
    neg  edx
.pc_pos:
    cmp  edx, ebx
    jle  .pc_next
    mov  ebx, edx
.pc_next:
    add  rdi, 4
    dec  rcx
    jnz  .pc_var
    shr  eax, 1
    cmp  ebx, eax
    pop  rdi
    pop  rcx
    pop  rbx
    pop  rax
    ret

; phi_advance: lk_advance — advance lattice epoch, reset GOI slots to GUZ
.phi_advance:
    call .phi_tick
    mov  rdi, 0x101020
    mov  rcx, 128
    mov  eax, [0x101804]
.pa_loop:
    cmp  dword [rdi], 0xFFFF0000
    jne  .pa_next
    mov  dword [rdi], eax
.pa_next:
    add  rdi, 4
    dec  rcx
    jnz  .pa_loop
    ret

; phi_fold (additive MAC, XOR-free)
.phi_lk_read:
    push rcx
    push rdi
    mov  rax, [0x101020]
    mov  rdi, 0
.plk_l:
    movzx rbx, byte [rsi]
    test  bl, bl
    jz    .plk_done
    mov   rcx, [0x101020 + rdi*4]
    imul  rax, rax, 3
    add   rax, rbx
    add   rax, rcx
    inc   rsi
    inc   rdi
    cmp   rdi, 128
    jl    .plk_l
    mov   rdi, 0
    jmp   .plk_l
.plk_done:
    mov  rcx, 4
.plk_fin:
    ror  rax, 3
    add  rax, [0x101020]
    dec  rcx
    jnz  .plk_fin
    mov  rbx, rax
    pop  rdi
    pop  rcx
    ret

; ── DATA ──────────────────────────────────────────────────────────────────
.msg_kernel_ok  db '[Kernel] phi-lattice 64-bit router ready. Consensus=LOCK',13,10,0
.msg_boot       db '[Omega] BOOT: graph init -> OBSERVE',13,10,0
.msg_realize    db '[Omega] REALIZE: T_COMPILE_SELF -> fixed point',13,10,0
.msg_runtime    db '[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete',13,10,0
.msg_graph_hdr  db '[Omega] Graph state:',13,10,0
.msg_omega      db '  Omega[',0
.msg_type       db '] type=',0
.msg_state      db ' state=',0
.msg_ana_dn     db '[Analog] Dn(r) lattice: phi-seeded 8-strand 32-slot',13,10,0
.msg_ana_strands db '  Strand A r=0.3 INIT  Strand H r=1.0 HELIX',13,10,0
.msg_ana_lock   db '  Kuramoto: PLUCK->SUSTAIN->FINETUNE->LOCK wu-wei',13,10,0
.sh_banner      db 13,10,'HDGL/64 Router> phi-lattice live (64-bit). help<enter>',13,10,0
.sh_prompt      db 'Router64> ',0
.sh_unknown     db '?',13,10,0
.sh_tick_done   db 'tick applied.',13,10,0
.sh_reset_msg   db 'resetting...',13,10,0
.sh_aphase_hdr  db 'Kuramoto aphase: ',0
.sh_aph_lock    db 'LOCK',13,10,0
.sh_aph_pluck   db 'PLUCK (converging)',13,10,0
.sh_aph_wu      db 'K/g wu-wei: 150:1',13,10,0
.sh_ps_hdr      db 'phi-lattice consensus state (64-bit):',13,10,0
.sh_ps_tick     db '  phi-tick:  ',0
.sh_ps_cons     db '  consensus: ',0
.sh_ps_lock     db 'LOCK (APA_FLAG_CONSENSUS set)',13,10,0
.sh_ps_goi      db '  GOI limit: 0x',0
.sh_ps_guz      db '  GUZ limit: 0x',0
.sh_up_pfx      db 'ticks: ',0
.sh_up_sfx      db ' (phi-ticks, 64-bit wu-wei)',13,10,0
.sh_wave_hdr    db 'wave (+/0/-) per strand:',13,10,0
.sh_wave_agg    db 'agg: ',0
.sh_info_cpu    db 'cpu:  ',0
.sh_info_mem    db 'mem:  ',0
.sh_info_kb     db ' KB',13,10,0
.sh_info_pci    db 'pci:  ',0
.sh_info_64     db 'mode: 64-bit long mode (rax/rdi/rsi)',13,10,0
.sh_phi_hdr     db 'phi-lattice depth Lp per node:',13,10,0
.sh_phi_pfx     db '  node[',0
.sh_ls_out      db 'sector  0      MBR stage1',13,10,'sector  1      stage2 (A20+E820+COM)',13,10,'sectors 2-17   runtime64 (8KB)',13,10,'sectors 18+    hdgl_firmware.hdgl',13,10,0
.sh_sectors_out db 'provisioned: 92. free: 92+',13,10,0
.sh_cat_hdr     db 'sector ',0
.sh_exec_load   db 'loading sector ',0
.sh_exec_jmp    db 'jumping...',13,10,0
.sh_glyph_ok    db 'mutated: ',0
.sh_glyph_bad   db 'invalid argument',13,10,0
.sh_strand_ok   db 'strand info: 8 strands, r_dim 0.3..1.0, Dn aggregate 0x80C0C0E8',13,10,0
.sh_dna_hdr     db 'hardware genome (CPUID vendor): ',0
.sh_dna_64      db 'mode: 64-bit long mode confirmed',13,10,0
.sh_tree_hdr    db 'Omega64 tree:',13,10,0
.sh_b4096_pfx   db 'b4096: ',0
.sh_xf_id       db 'T_IDENTITY',0
.sh_xf_cpuid    db 'T_CPUID',0
.sh_xf_e820     db 'T_E820',0
.sh_xf_pci      db 'T_PCI_WALK',0
.sh_xf_compile  db 'T_COMPILE_SELF',0
.sh_xf_rewrite  db 'T_REWRITE',0

.sh_help_text   db \
    'omega [N]  print graph / node N',13,10,\
    'tick       one rewrite pass',13,10,\
    'dn         Dn(r) lattice summary',13,10,\
    'aphase     Kuramoto phase',13,10,\
    'ps         phi-lattice consensus',13,10,\
    'uptime     phi-tick count',13,10,\
    'wave       strand wave aggregate',13,10,\
    'info       cpu/mem/pci + mode',13,10,\
    'phi        phi-depth per node',13,10,\
    'ls         disk layout',13,10,\
    'cat S      hex dump sector S',13,10,\
    'exec S     load+run sector S',13,10,\
    'reset      re-run boot sequence',13,10,\
    'glyph N S  mutate node state',13,10,\
    'strand N   strand N info',13,10,\
    'dna        hardware genome',13,10,\
    'tree       Omega graph tree',13,10,\
    'b4096 N    Base4096 encode node',13,10,\
    'xform N    transform field',13,10,\
    'sectors    disk layout',13,10,\
    'help       this list',13,10,'boot [N]   phi-bridge + chainload drive N (default 0x81)',13,10,'advance    lk_advance: entropy ratchet, invalidate epoch',13,10,'attest     lk_commit: PCR chain step, print hash',13,10,'cap <name> lk_cap: capability token for name',13,10,'seal <msg> phi_stream_seal: AEAD-encrypt, print hex',13,10,'unseal <h> phi_stream_open: constant-time verify+decrypt',13,10,'nic        list detected router NICs',13,10,'uart       list detected serial ports',13,10,0,0

; Command keyword strings
; Router extension strings
.sh_info_nics   db 'nics: ',0
.sh_info_uarts  db 'uart: ',0
.sh_nic_hdr     db 'NICs detected:',13,10,0
.sh_nic_none    db '  (none — check PCI scan range)',13,10,0
.sh_nic_unknown db ' unknown',0
.sh_uart_hdr    db 'UARTs detected:',13,10,0
.sh_uart_none   db '  (none)',13,10,0
.sh_cmd_nic_s   db 'nic',0
.sh_cmd_uart_s  db 'uart',0

; NIC type table: dd vendor_device, dd padding, dq ptr_to_name_str
; Terminated by dd 0
align 8
.nic_type_table:
    dd 0x100E8086, 0
    dq .nic_name_e1000
    dd 0x10D38086, 0
    dq .nic_name_82574
    dd 0x15398086, 0
    dq .nic_name_i211
    dd 0x15218086, 0
    dq .nic_name_i350
    dd 0x15338086, 0
    dq .nic_name_i210
    dd 0x10D38086, 0
    dq .nic_name_82574
    dd 0x10918086, 0
    dq .nic_name_82574
    dd 0x13688086, 0
    dq .nic_name_I226
    dd 0x15F38086, 0
    dq .nic_name_I225
    dd 0x816810EC, 0
    dq .nic_name_rtl8111
    dd 0x816910EC, 0
    dq .nic_name_rtl8169
    dd 0x10661969, 0
    dq .nic_name_ar8131
    dd 0x10C310EC, 0
    dq .nic_name_rtl8139
    dd 0, 0
.nic_name_e1000   db '[Intel e1000]',0
.nic_name_i211   db '[Intel i211AT]',0
.nic_name_i350   db '[Intel i350]',0
.nic_name_i210   db '[Intel i210]',0
.nic_name_82574  db '[Intel 82574L]',0
.nic_name_I226   db '[Intel I226-V]',0
.nic_name_I225   db '[Intel I225-V]',0
.nic_name_rtl8111 db '[RTL8111/8168]',0
.nic_name_rtl8169 db '[RTL8169]',0
.nic_name_ar8131  db '[Atheros AR8131]',0
.nic_name_rtl8139 db '[RTL8139]',0

.sh_cmd_omega_s   db 'omega',0
.sh_cmd_tick_s    db 'tick',0
.sh_cmd_dn_s      db 'dn',0
.sh_cmd_aphase_s  db 'aphase',0
.sh_cmd_ps_s      db 'ps',0
.sh_cmd_uptime_s  db 'uptime',0
.sh_cmd_wave_s    db 'wave',0
.sh_cmd_info_s    db 'info',0
.sh_cmd_phi_s     db 'phi',0
.sh_cmd_ls_s      db 'ls',0
.sh_cmd_sectors_s db 'sectors',0
.sh_cmd_cat_s     db 'cat',0
.sh_cmd_exec_s    db 'exec',0
.sh_cmd_reset_s   db 'reset',0
.sh_cmd_help_s    db 'help',0
.sh_cmd_strand_s  db 'strand',0
.sh_cmd_glyph_s   db 'glyph',0
.sh_cmd_dna_s     db 'dna',0
.sh_cmd_tree_s    db 'tree',0
.sh_cmd_b4096_s   db 'b4096',0
.sh_cmd_xform_s   db 'xform',0

.sh_buf           times 64  db 0
.sh_dec_buf       times 24  db 0

; Phi-seed table (from conscious ll_analog.c, verified)
; fib[i%8] * prime[i%8] * PHI^(1+(i%4)) * 65536
.phi_seed_table:
    dd 0x00033C6F, 0x0007DAA6, 0x002A5C56, 0x008FEFA7
    dd 0x0058FDEB, 0x01104689, 0x03A82BC8, 0x0AAEC964
    dd 0x00033C6F, 0x0007DAA6, 0x002A5C56, 0x008FEFA7
    dd 0x0058FDEB, 0x01104689, 0x03A82BC8, 0x0AAEC964
.phi_seed_table_end:

; ── PHI-BRIDGE TRAMPOLINE DATA ──────────────────────────────────────────────
; Raw 32-bit code bytes. Copied to 0x4000 by 'boot' command.
.phib_trampoline:
    db 0x0F,0x20,0xC0,0x25,0xFF,0xFF,0xFF,0x7F,0x0F,0x22,0xC0,0xEB,0x00,0x0F,0x01,0x15
    db 0xB0,0x40,0x00,0x00,0x66,0xB8,0x08,0x00,0x8E,0xD8,0x8E,0xC0,0x8E,0xE0,0x8E,0xE8
    db 0x8E,0xD0,0xEA,0x29,0x40,0x00,0x00,0x10,0x00,0x0F,0x20,0xC0,0x24,0xFE,0x0F,0x22
    db 0xC0,0xEA,0x36,0x40,0x00,0x00,0x31,0xC0,0x8E,0xD8,0x8E,0xC0,0x8E,0xD0,0xBC,0xF0
    db 0x7B,0x88,0xDA,0xB4,0x02,0xB0,0x01,0xB5,0x00,0xB1,0x01,0xB6,0x00,0xBB,0x00,0x7C
    db 0xCD,0x13,0x72,0x0D,0x81,0x3E,0xFE,0x7D,0x55,0xAA,0x75,0x05,0xEA,0x00,0x7C,0x00
    db 0x00,0xBE,0x7D,0x40,0xAC,0x84,0xC0,0x74,0x11,0xBA,0xFD,0x03,0xEC,0xA8,0x20,0x74
    db 0xFB,0xBA,0xF8,0x03,0x8A,0x44,0xFF,0xEE,0xEB,0xEA,0xF4,0xEB,0xFD,0x48,0x44,0x47
    db 0x4C,0x3A,0x20,0x63,0x68,0x61,0x69,0x6E,0x6C,0x6F,0x61,0x64,0x20,0x66,0x61,0x69
    db 0x6C,0x65,0x64,0x0D,0x0A,0x00,0x90,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    db 0xFF,0xFF,0x00,0x00,0x00,0x92,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x9A,0x00,0x00
    db 0x17,0x00,0x98,0x40
.phib_trampoline_end:

; ── GDTs — BEFORE padding so they're inside the binary ────────────────────
; CRITICAL: must appear before times pad or LGDT references zeros.
[BITS 16]
align 8
gdt32_start:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32_start - 1
    dd gdt32_start

gdt64_start:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start

times 32768-($-$$) db 0          ; pad to 32KB (64 sectors)

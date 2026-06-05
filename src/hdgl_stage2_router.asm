; ============================================================
; HDGL STAGE2 — sector 1 (512 bytes, loaded to 0x7E00)
; Runs in real mode. Full gap-closure:
;   A20 verify + KBC fallback
;   E820 memory map (must be real-mode, before PM)
;   COM probe: 0x3F8 → 0x2F8 → silent
;   LBA/CHS runtime load (sectors 2-17 → 0x8000)
;   Then hands off to firmware_runtime at 0x8000
; phi-neutral: mov not xor. No UEFI.
; ============================================================
[bits 16]
[org 0x7E00]

    ; DL = drive number (passed from MBR)
    mov [.drive], dl

    ; ── A20: verify, then KBC if needed ──────────────────────
    call .a20_test
    jnz .a20_verified
    ; Fast-gate already tried in MBR; go straight to KBC
    call .kbc_a20
.a20_verified:

    ; ── E820 memory map → 0x500, count → 0x4F8 ──────────────
    call .do_e820

    ; ── COM probe → 0x7FEC ───────────────────────────────────
    call .probe_com

    ; ── Load runtime: sectors 2-17 → 0x8000 ─────────────────
    ; (Runtime org is 0x8000, not 0x7E00 — avoids stage2 overlap)
    call .load_runtime
    jc .halt

    ; ── Load source: sectors 19-22 → 0xA000 ─────────────────
    call .load_source

    ; Pass boot info to runtime
    mov word [0x7FF0], 0xA000
    mov word [0x7FF2], 0x0000
    mov word [0x7FEE], 0x8000   ; runtime load address

    jmp 0x0000:0x8000

.halt:
    cli
    hlt
    jmp .halt

; ── A20 test: ZF=0 if A20 ON ─────────────────────────────
.a20_test:
    push es
    push bx
    mov  bx, 0xFFFF
    mov  es, bx
    mov  word [0x0500], 0xAA55
    mov  bx, [es:0x0510]
    cmp  bx, 0xAA55
    je   .a20_off           ; same value wraps → A20 off
    pop  bx
    pop  es
    mov  ax, 1
    test ax, ax             ; ZF=0 → A20 on
    ret
.a20_off:
    pop  bx
    pop  es
    mov  ax, 0
    test ax, ax             ; ZF=1 → A20 off
    ret

; ── KBC A20 enable ───────────────────────────────────────
.kbc_a20:
    call .kbc_in
    mov  al, 0xAD
    out  0x64, al           ; disable keyboard
    call .kbc_in
    mov  al, 0xD0
    out  0x64, al           ; read output port
    call .kbc_out
    in   al, 0x60
    push ax
    call .kbc_in
    mov  al, 0xD1
    out  0x64, al           ; write output port
    call .kbc_in
    pop  ax
    or   al, 0x02
    out  0x60, al           ; set A20
    call .kbc_in
    mov  al, 0xAE
    out  0x64, al           ; re-enable keyboard
    ret
.kbc_in:
    in   al, 0x64
    test al, 0x02
    jnz  .kbc_in
    ret
.kbc_out:
    in   al, 0x64
    test al, 0x01
    jz   .kbc_out
    ret

; ── E820 ──────────────────────────────────────────────────
.do_e820:
    mov  word [0x4F8], 0
    mov  di, 0x500
    mov  ebx, 0
    mov  edx, 0x534D4150
.e820l:
    mov  eax, 0xE820
    mov  ecx, 24
    mov  dword [di+20], 1
    int  0x15
    jc   .e820d
    cmp  eax, 0x534D4150
    jne  .e820d
    jcxz .e820s
    mov  eax, [di+8]
    or   eax, [di+12]
    jz   .e820s
    add  di, 24
    inc  word [0x4F8]
.e820s:
    test ebx, ebx
    jnz  .e820l
.e820d:
    ret

; ── COM probe ─────────────────────────────────────────────
.probe_com:
    mov  word [0x7FEC], 0
    ; Try COM1 0x3F8
    mov  dx, 0x3F9
    mov  al, 0
    out  dx, al
    in   al, dx
    test al, 0xFF
    jnz  .try2
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
    mov  dx, 0x3FA
    mov  al, 0xC7
    out  dx, al
    mov  word [0x7FEC], 0x3F8
    ret
.try2:
    ; Try COM2 0x2F8
    mov  dx, 0x2F9
    mov  al, 0
    out  dx, al
    in   al, dx
    test al, 0xFF
    jnz  .nocom
    mov  dx, 0x2FB
    mov  al, 0x80
    out  dx, al
    mov  dx, 0x2F8
    mov  al, 12
    out  dx, al
    mov  dx, 0x2F9
    mov  al, 0
    out  dx, al
    mov  dx, 0x2FB
    mov  al, 0x03
    out  dx, al
    mov  dx, 0x2FA
    mov  al, 0xC7
    out  dx, al
    mov  word [0x7FEC], 0x2F8
.nocom:
    ret

; ── Load runtime: sectors 2-17 → 0x8000 ──────────────────
.load_runtime:
    cmp  byte [.drive+1], 1  ; .lba flag
    je   .rt_lba
    mov  ah, 0x02
    mov  al, 32
    mov  ch, 0
    mov  cl, 3               ; sector 2 (CHS: C=0 H=0 S=2+1=3? no: sector 3 on disk)
    mov  dh, 0
    mov  dl, [.drive]
    mov  bx, 0x8000
    int  0x13
    ret
.rt_lba:
    mov  dword [.dap+8],  2
    mov  dword [.dap+12], 0
    mov  word  [.dap+2],  32
    mov  word  [.dap+4],  0x8000
    mov  word  [.dap+6],  0
    mov  ah, 0x42
    mov  dl, [.drive]
    mov  si, .dap
    int  0x13
    ret

; ── Load source: sectors 19-22 → 0xA000 ──────────────────
.load_source:
    cmp  byte [.drive+1], 1
    je   .src_lba
    ; CHS fallback: skip (source load optional)
    ret
.src_lba:
    mov  dword [.dap+8],  19
    mov  dword [.dap+12], 0
    mov  word  [.dap+2],  4
    mov  word  [.dap+4],  0xA000
    mov  word  [.dap+6],  0
    mov  ah, 0x42
    mov  dl, [.drive]
    mov  si, .dap
    int  0x13
    ret

.drive  db 0x80
.lba    db 0
.dap    db 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

times 512-($-$$) db 0

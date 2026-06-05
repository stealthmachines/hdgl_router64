; ============================================================
; HDGL MBR — sector 0 (512 bytes)
; Loads stage2 stub from sector 1 to 0x7E00, jumps there.
; Stage2 has the full space it needs for E820+A20+COM+LBA.
; phi-neutral: mov not xor throughout.
; ============================================================
[bits 16]
[org 0x7C00]

    cli
    mov ax, 0
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7BF0
    sti
    mov [.drive], dl        ; save BIOS drive number

    ; A20 fast-gate (quick path — stage2 will verify + KBC fallback)
    in  al, 0x92
    test al, 0x02
    jnz .a20_done
    or  al, 0x02
    and al, 0xFE
    out 0x92, al
.a20_done:

    ; Load stage2 (sector 1, 1 sector → 0x7E00)
    ; Try LBA first
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc  .chs
    cmp bx, 0xAA55
    jne .chs
    ; LBA
    mov dword [.dap+8],  1
    mov dword [.dap+12], 0
    mov word  [.dap+2],  1
    mov word  [.dap+4],  0x7E00
    mov word  [.dap+6],  0
    mov ah, 0x42
    mov dl, [.drive]
    mov si, .dap
    int 0x13
    jc  .halt
    jmp .go
.chs:
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [.drive]
    mov bx, 0x7E00
    int 0x13
    jc  .halt
.go:
    mov dl, [.drive]
    jmp 0x0000:0x7E00

.halt:
    cli
    hlt
    jmp .halt

.drive db 0x80
.dap   db 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

times 510-($-$$) db 0
dw 0xAA55

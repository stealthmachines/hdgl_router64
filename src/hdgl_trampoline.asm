; HDGL phi-bridge trampoline — hdgl_trampoline.asm
; Assembled to binary and embedded as raw bytes in hdgl_router64.asm.
; The 'boot' command copies these bytes to 0x4000 and far-jumps there.
;
; Entry: EBX = target BIOS drive number (0x80, 0x81, ...)
; Runs in 32-bit protected mode (CR0.PE=1, paging enabled).
; Disables paging, enters 16-bit real mode, chainloads target MBR.
;
; Build: nasm -f bin hdgl_trampoline.asm -o hdgl_trampoline.bin

[BITS 32]
[ORG 0x4000]

    ; Disable paging
    mov  eax, cr0
    and  eax, 0x7FFFFFFF
    mov  cr0, eax
    jmp  .flush
.flush:
    ; Load 16-bit GDT
    lgdt [.gdt16_ptr]
    mov  ax, 0x08
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    jmp  0x10:.mode16

[BITS 16]
.mode16:
    ; Clear PE
    mov  eax, cr0
    and  al, 0xFE
    mov  cr0, eax
    jmp  0x0000:.realmode
.realmode:
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7BF0
    ; Load target MBR via INT 13h
    mov  dl, bl
    mov  ah, 0x02
    mov  al, 1
    mov  ch, 0
    mov  cl, 1
    mov  dh, 0
    mov  bx, 0x7C00
    int  0x13
    jc   .fail
    cmp  word [0x7DFE], 0xAA55
    jne  .fail
    jmp  0x0000:0x7C00
.fail:
    mov  si, .msg
.pl: lodsb
    test al, al
    jz   .halt
    mov  dx, 0x3FD
.pw: in al, dx
    test al, 0x20
    jz   .pw
    mov  dx, 0x3F8
    mov  al, [si-1]
    out  dx, al
    jmp  .pl
.halt: hlt
    jmp  .halt
.msg db 'HDGL: chainload failed',13,10,0

align 8
.gdt16:
    dq   0
    dw   0xFFFF, 0x0000, 0x9200, 0x0000
    dw   0xFFFF, 0x0000, 0x9A00, 0x0000
.gdt16_end:
.gdt16_ptr:
    dw   .gdt16_end - .gdt16 - 1
    dd   .gdt16

times 512-($-$$) db 0

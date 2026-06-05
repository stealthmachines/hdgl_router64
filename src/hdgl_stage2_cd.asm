[bits 16]
[org 0x7E00]
    mov  word [0x7FEC], 0x3F8   ; COM1 base for runtime64
    jmp  0x0000:0x8000
times 512-($-$$) db 0

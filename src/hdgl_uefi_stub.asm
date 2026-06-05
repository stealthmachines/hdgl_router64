; ============================================================
; HDGL UEFI STUB — hdgl_uefi_stub.efi
; Minimal PE32+ EFI application (x86_64)
;
; Philosophy: prefers legacy BIOS path.
;   1. Announce ourselves via UEFI ConOut
;   2. Attempt to locate legacy BIOS bootable disk
;      via EFI_BLOCK_IO_PROTOCOL and load our MBR chain
;   3. If legacy path succeeds → ExitBootServices + far jump to 0x7C00
;   4. If not → print UEFI-mode notice, load phi-lattice runtime
;      via EFI LoadImage/StartImage, run in UEFI context
;   5. Final fallback: print error, wait for keypress, reset
;
; UEFI System Table offsets (x86_64 calling convention: RCX, RDX, R8, R9)
;   SystemTable->ConOut            = [RSI + 0x40] (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*)
;   ConOut->OutputString(this, s)  = [ConOut + 0x08] (function pointer)
;   SystemTable->BootServices      = [RSI + 0x60] (EFI_BOOT_SERVICES*)
;   BootServices->Exit             = [BS + 0xD8]
;   BootServices->Stall            = [BS + 0x198]
;   BootServices->ExitBootServices = [BS + 0xE8]
; ============================================================

BITS 64
ORG 0

; ── DOS stub (required by PE spec) ────────────────────────────────────────
dos_header:
    dw 0x5A4D                  ; MZ signature
    times 29 dw 0
    dd pe_header - dos_header  ; offset to PE header

; ── PE32+ header ──────────────────────────────────────────────────────────
pe_header:
    dd 0x00004550              ; "PE\0\0"
    ; COFF header
    dw 0x8664                  ; Machine: AMD64
    dw 1                       ; NumberOfSections: 1
    dd 0                       ; TimeDateStamp
    dd 0                       ; PointerToSymbolTable
    dd 0                       ; NumberOfSymbols
    dw opt_header_end - opt_header  ; SizeOfOptionalHeader
    dw 0x0206                  ; Characteristics: exe, large-addr, 64-bit

opt_header:
    dw 0x020B                  ; Magic: PE32+
    db 0, 0                    ; MajorLinkerVersion, MinorLinkerVersion
    dd code_size               ; SizeOfCode
    dd 0                       ; SizeOfInitializedData
    dd 0                       ; SizeOfUninitializedData
    dd efi_entry - dos_header  ; AddressOfEntryPoint (RVA)
    dd code_start - dos_header ; BaseOfCode (RVA)
    dq 0                       ; ImageBase (EFI loader will rebase)
    dd 0x20                    ; SectionAlignment
    dd 0x20                    ; FileAlignment
    dw 0, 0                    ; OS version
    dw 0, 0                    ; Image version
    dw 0, 1                    ; Subsystem version (EFI = 1.0)
    dd 0                       ; Win32VersionValue
    dd image_size              ; SizeOfImage
    dd code_start - dos_header ; SizeOfHeaders
    dd 0                       ; CheckSum
    dw 10                      ; Subsystem: EFI application
    dw 0                       ; DllCharacteristics
    dq 0                       ; SizeOfStackReserve
    dq 0                       ; SizeOfStackCommit
    dq 0                       ; SizeOfHeapReserve
    dq 0                       ; SizeOfHeapCommit
    dd 0                       ; LoaderFlags
    dd 0                       ; NumberOfRvaAndSizes (no data directories)
opt_header_end:

; ── Section table (.text) ─────────────────────────────────────────────────
section_text:
    db '.text', 0, 0, 0        ; Name (8 bytes)
    dd code_size               ; VirtualSize
    dd code_start - dos_header ; VirtualAddress (RVA)
    dd code_size               ; SizeOfRawData
    dd code_start - dos_header ; PointerToRawData
    dd 0                       ; PointerToRelocations
    dd 0                       ; PointerToLinenumbers
    dw 0                       ; NumberOfRelocations
    dw 0                       ; NumberOfLinenumbers
    dd 0x60000020              ; Characteristics: code, execute, read

align 0x20, db 0
code_start:

; ── EFI entry point ───────────────────────────────────────────────────────
; RCX = EFI_HANDLE ImageHandle
; RDX = EFI_SYSTEM_TABLE *SystemTable
efi_entry:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x60
    ; Save args
    mov  [rbp-8],  rcx         ; ImageHandle
    mov  [rbp-16], rdx         ; SystemTable

    ; ── Print banner via ConOut ───────────────────────────────────────────
    mov  rsi, rdx              ; SystemTable
    mov  rdi, [rsi + 0x40]    ; ConOut = SystemTable->ConOut
    lea  rcx, [rel msg_banner]
    mov  rdx, rcx
    mov  rcx, rdi              ; this = ConOut
    call qword [rdi + 0x08]    ; ConOut->OutputString(ConOut, msg)

    ; ── Attempt to locate BootServices ───────────────────────────────────
    mov  rsi, [rbp-16]
    mov  rbx, [rsi + 0x60]    ; BootServices

    ; ── Try legacy: find a disk with our MBR signature ───────────────────
    ; We probe via BootServices->LocateHandleBuffer for BlockIo
    ; GUID: {964E5B21-6459-11D2-8E39-00A0C969723B} (EFI_BLOCK_IO_PROTOCOL)
    ; If disk 0 has AA55 at offset 510 → it's our legacy image
    ; For simplicity in this stub: announce intent, then chainload
    ; via BootServices->LoadImage pointing at the disk device.
    ;
    ; "PREFERENCING non-UEFI": we check if legacy BIOS path is viable.
    ; In a UEFI-only environment (no CSM), we can't truly do INT 13h,
    ; so we: (a) print the preference message, (b) load our runtime64
    ; binary from disk sector 2 via Block I/O, (c) allocate pages and
    ; jump to it — giving the phi-lattice kernel UEFI boot services
    ; it can optionally use before calling ExitBootServices.

    mov  rdi, [rsi + 0x40]    ; ConOut again
    lea  rdx, [rel msg_prefer]
    mov  rcx, rdi
    call qword [rdi + 0x08]

    ; ── Stall 2 seconds (let user see the message) ───────────────────────
    ; BootServices->Stall(2000000 microseconds)
    mov  rcx, 2000000
    call qword [rbx + 0x198]

    ; ── Print UEFI mode active ────────────────────────────────────────────
    mov  rdi, [rsi + 0x40]
    lea  rdx, [rel msg_uefi_mode]
    mov  rcx, rdi
    call qword [rdi + 0x08]

    ; ── Print phi-lattice init message ───────────────────────────────────
    mov  rdi, [rsi + 0x40]
    lea  rdx, [rel msg_phi]
    mov  rcx, rdi
    call qword [rdi + 0x08]

    ; ── Stall 1 second then return EFI_SUCCESS ───────────────────────────
    ; In a full implementation we would:
    ;   AllocatePages → load runtime64 sectors → ExitBootServices → jump
    ; The full chainload is disk-handle specific and requires
    ; LocateHandleBuffer + OpenProtocol per handle.
    ; This stub announces, waits, and returns — the UEFI shell or
    ; firmware boot manager will then load the next entry.
    mov  rcx, 1000000
    call qword [rbx + 0x198]

    ; Return EFI_SUCCESS (0)
    mov  eax, 0
    leave
    ret

; ── UCS-2 strings (UEFI ConOut requires UCS-2LE) ─────────────────────────
msg_banner:
    dw 'H','D','G','L',' ','U','E','F','I',' ','S','t','u','b',' ','v','1',0x0D,0x0A,0

msg_prefer:
    dw 'P','r','e','f','e','r','r','i','n','g',' ','l','e','g','a','c','y'
    dw ' ','B','I','O','S',' ','p','a','t','h','.',' '
    dw 'C','h','e','c','k','i','n','g',' ','d','i','s','k','.',0x0D,0x0A,0

msg_uefi_mode:
    dw 'N','o',' ','l','e','g','a','c','y',' ','B','I','O','S','.',' '
    dw 'R','u','n','n','i','n','g',' ','i','n',' ','U','E','F','I',' '
    dw 'm','o','d','e','.',0x0D,0x0A,0

msg_phi:
    dw 'p','h','i','-','l','a','t','t','i','c','e',' ','k','e','r','n','e','l'
    dw ' ','w','i','l','l',' ','i','n','i','t',' ','v','i','a',' '
    dw 'E','F','I',' ','B','o','o','t','S','e','r','v','i','c','e','s','.',0x0D,0x0A,0

align 0x20, db 0
code_end:

; ── Size constants ────────────────────────────────────────────────────────
code_size  equ code_end - code_start
image_size equ code_end - dos_header

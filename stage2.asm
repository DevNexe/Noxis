[BITS 16]

global stage2_start
extern kmain
extern setup_paging

FS_LOAD_SEGMENT equ 0x5000
FS_LOAD_OFFSET  equ 0x0000
FS_LBA_START    equ 64
FS_SECTORS      equ 64

stage2_start:
    cli

    mov [boot_drive], dl

    mov ah, 0x0E
    mov al, '2'
    int 0x10

    ; Load NoxisFS image to physical 0x50000 while BIOS is available.
    xor ax, ax
    mov ds, ax
    mov si, fs_dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc fs_load_error

    ; A20 enable
    in al, 0x92
    or al, 2
    out 0x92, al

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode

fs_load_error:
    mov ah, 0x0E
    mov al, 'F'
    int 0x10
    cli
    hlt

[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov byte [0xB8000], '3'
    mov byte [0xB8001], 0x0F

    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    call setup_paging

    mov byte [0xB8002], '4'
    mov byte [0xB8003], 0x0F

    mov eax, 0x1000
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    mov byte [0xB8004], '5'
    mov byte [0xB8005], 0x0F

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    jmp 0x18:long_mode

[BITS 64]
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, 0x90000

    mov byte [0xB8006], '6'
    mov byte [0xB8007], 0x0F

    call kmain
    hlt

align 8
boot_drive: db 0

fs_dap:
    db 0x10
    db 0x00
    dw FS_SECTORS
    dw FS_LOAD_OFFSET
    dw FS_LOAD_SEGMENT
    dq FS_LBA_START

align 8
gdt_start:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00AF9A000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dq gdt_start

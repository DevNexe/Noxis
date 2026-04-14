[BITS 16]
[ORG 0x7C00]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov ah, 0x0E
    mov al, 'K'
    int 0x10

    mov [boot_drive], dl

    mov si, disk_address_packet
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    jmp 0x8000

disk_error:
    mov ah, 0x0E
    mov al, 'E'
    int 0x10
    cli
    hlt

boot_drive: db 0

disk_address_packet:
    db 0x10
    db 0x00
    dw 96              ; stage2 sectors from LBA 1
    dw 0x8000          ; offset
    dw 0x0000          ; segment
    dq 1               ; LBA start

times 510 - ($ - $$) db 0
dw 0xAA55


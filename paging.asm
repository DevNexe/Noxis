[BITS 32]

global setup_paging

; Таблица страниц по адресу 0x1000
; PML4 = 0x1000, PDP = 0x2000, PD = 0x3000

setup_paging:
    ; Очищаем 3 страницы
    mov edi, 0x1000
    mov ecx, 0x3000 / 4
    xor eax, eax
    rep stosd

    ; PML4[0] -> PDP
    mov dword [0x1000], 0x2003

    ; PDP[0] -> PD
    mov dword [0x2000], 0x3003

    ; PD[0] -> 2MB huge page
    mov dword [0x3000], 0x83

    ret
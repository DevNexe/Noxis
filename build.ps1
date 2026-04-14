$ErrorActionPreference = 'Stop'

x86_64-elf-g++ -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -c kernel.cpp -o kernel.o
nasm -f elf64 stage2.asm -o stage2.o
nasm -f elf64 paging.asm -o paging.o
nasm -f bin boot.asm -o boot.bin
x86_64-elf-ld -T linker.ld -o stage2.elf stage2.o paging.o kernel.o
x86_64-elf-objcopy -O binary stage2.elf stage2.bin

.\mkfs.ps1

$stage2Sectors = [math]::Ceiling(([System.IO.FileInfo]::new('stage2.bin').Length) / 512)
if ($stage2Sectors -gt 96) {
    throw "stage2.bin is too large ($stage2Sectors sectors), boot loader reads 96 sectors"
}

$imgSize = 512 * 2880
[byte[]]$img = New-Object byte[] $imgSize
[byte[]]$boot = [System.IO.File]::ReadAllBytes('boot.bin')
[byte[]]$stage2 = [System.IO.File]::ReadAllBytes('stage2.bin')
[byte[]]$fs = [System.IO.File]::ReadAllBytes('fs.bin')

[Array]::Copy($boot, 0, $img, 0, [Math]::Min($boot.Length, 512))
[Array]::Copy($stage2, 0, $img, 512, [Math]::Min($stage2.Length, $imgSize - 512))

$fsOffset = 128 * 512
[Array]::Copy($fs, 0, $img, $fsOffset, [Math]::Min($fs.Length, $imgSize - $fsOffset))

[System.IO.File]::WriteAllBytes('os.img', $img)
Write-Host "Built os.img (stage2 sectors=$stage2Sectors, fs at LBA 128)"

all: os.img

kernel.o: kernel.cpp
	x86_64-elf-g++ -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -c kernel.cpp -o kernel.o

stage2.o: stage2.asm
	nasm -f elf64 stage2.asm -o stage2.o

paging.o: paging.asm
	nasm -f elf64 paging.asm -o paging.o

stage2.bin: stage2.o paging.o kernel.o
	x86_64-elf-ld -T linker.ld -o stage2.elf stage2.o paging.o kernel.o
	x86_64-elf-objcopy -O binary stage2.elf stage2.bin

boot.bin: boot.asm
	nasm -f bin boot.asm -o boot.bin

fs.bin: mkfs.ps1
	powershell -ExecutionPolicy Bypass -File .\mkfs.ps1

os.img: boot.bin stage2.bin fs.bin
	powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1

run: os.img
	qemu-system-x86_64 -drive format=raw,file=os.img

clean:
	rm -f *.o *.bin *.elf *.img

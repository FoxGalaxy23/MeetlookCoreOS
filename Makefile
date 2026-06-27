CC      = gcc
CFLAGS  = -m16 -ffreestanding -fno-pie -fno-stack-protector \
          -nostdlib -O1 -Wall
NASM    = nasm
NASMFMT = -f elf

all: hello.bin

boot.o: boot.asm
	$(NASM) $(NASMFMT) boot.asm -o boot.o

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

hello.bin: boot.o main.o linker.ld
	ld -m elf_i386 -T linker.ld boot.o main.o -o hello.bin

run: hello.bin
	qemu-system-x86_64 -drive format=raw,file=hello.bin

clean:
	rm -f *.o *.bin qemu.log
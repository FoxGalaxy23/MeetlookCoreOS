CC      = gcc
CFLAGS  = -m16 -ffreestanding -fno-pie -fno-stack-protector \
          -nostdlib -O1 -Wall -Wno-unused-function
NASM    = nasm

# Итоговый образ диска
IMAGE   = meetlookcoreos.img

all: $(IMAGE)

# Загрузчик: плоский бинарный формат (512 байт)
boot.bin: boot.asm
	$(NASM) -f bin boot.asm -o boot.bin

# Точка входа ядра (обязательно перед main.o)
kernel_entry.o: kernel_entry.asm
	$(NASM) -f elf kernel_entry.asm -o kernel_entry.o

# Ядро: компилируем C → obj
main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

# Ядро: линкуем в плоский бинарник (загружается в 0x1000)
kernel.bin: kernel_entry.o main.o linker.ld
	ld -m elf_i386 -T linker.ld kernel_entry.o main.o -o kernel.bin

# Финальный образ: boot.bin + kernel.bin, добитый до 1.44MB
$(IMAGE): boot.bin kernel.bin
	dd if=/dev/zero    of=$(IMAGE) bs=512 count=2880 2>/dev/null
	dd if=boot.bin     of=$(IMAGE) bs=512 count=1 conv=notrunc 2>/dev/null
	dd if=kernel.bin   of=$(IMAGE) bs=512 seek=1 conv=notrunc 2>/dev/null
	@echo ">> Image ready: $(IMAGE)"
	@echo "   boot.bin  : $(shell stat -c%s boot.bin) bytes"
	@echo "   kernel.bin: $(shell stat -c%s kernel.bin) bytes"

run: $(IMAGE)
	qemu-system-x86_64 -drive format=raw,file=$(IMAGE) -display sdl 2>/dev/null || \
	qemu-system-x86_64 -drive format=raw,file=$(IMAGE) -display gtk 2>/dev/null || \
	qemu-system-x86_64 -drive format=raw,file=$(IMAGE)

run-curses: $(IMAGE)
	qemu-system-x86_64 -drive format=raw,file=$(IMAGE) -display curses

clean:
	rm -f *.o *.bin *.img qemu.log

.PHONY: all run run-curses clean
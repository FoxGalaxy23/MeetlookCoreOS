CC = gcc
AS = nasm
LD = ld

GCC_INCLUDE = $(shell gcc -m64 -print-file-name=include)

CFLAGS = -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone \
         -mcmodel=small -nostdlib -nostdinc \
         -Wall -Wextra -O2 -Iinclude -isystem $(GCC_INCLUDE)
ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -T boot/linker.ld -nostdlib -z max-page-size=0x1000

SRCS_C = kernel/kernel.c kernel/kprintf.c kernel/console.c kernel/power.c \
         kernel/loader.c kernel/pmm.c kernel/kheap.c kernel/users.c kernel/userdisk.c \
         kernel/bootconfirm.c \
         kernel/idt.c kernel/isr.c kernel/pic.c kernel/timer.c kernel/task.c \
         kernel/sync.c kernel/ipc.c \
         kernel/gdt.c kernel/paging.c kernel/process.c kernel/syscall.c \
         kernel/container.c kernel/irqbridge.c kernel/netbridge.c kernel/netdrv.c \
         kernel/uaccess.c \
         kernel/spinlock.c kernel/percpu.c kernel/apic.c kernel/acpi.c kernel/smp.c \
         kernel/bootinfo.c kernel/bootinfo_multiboot.c kernel/bootinfo_e820.c \
         kernel/boot_assets.c kernel/boot_assets_data.c \
         drivers/vga.c drivers/keyboard.c drivers/rtc.c \
         drivers/pci.c drivers/storage.c drivers/ide.c drivers/ahci.c \
         drivers/usb.c drivers/usb_uhci.c drivers/usb_hid.c drivers/usb_msd.c \
         net/net.c net/eth.c net/arp.c net/ip.c net/icmp.c net/udp.c \
         net/dns.c net/dhcp.c net/tcp.c \
         fs/fat32.c fs/ufs.c fs/ramfs.c fs/ramfs_data.c fs/vfs.c lib/string.c \
         lib/mbr.c \
         lib/crypto_sha256.c lib/crypto_hmac.c lib/crypto_aes.c lib/crypto_xts.c lib/cryptoblk.c \
         lib/gpt.c lib/crc32.c lib/crypto_rng.c lib/crypto_pbkdf2.c lib/userimg.c lib/bootmeta.c
         # lib/mbr.c нужен здесь (не только приложениям вроде mbrtest/install-os)
         # с тех пор, как fs/fat32.c стал partition-aware (INSTALLER_PLAN.md,
         # Фаза 3): detect_partition_range() внутри fat32.c зовёт mbr_parse()/
         # mbr_partition_is_used() напрямую.
         # lib/crypto_sha256.c/crypto_hmac.c/crypto_aes.c/crypto_xts.c/cryptoblk.c
         # нужны здесь с тех пор, как fs/ufs.c стал уметь монтировать тело
         # зашифрованного пользовательского образа (ATOMIC_PLAN.md, Фаза 4,
         # ufs_mount_encrypted() -- см. её общий комментарий в fs/ufs.c).
         # lib/gpt.c/crc32.c/crypto_rng.c/crypto_pbkdf2.c/userimg.c/bootmeta.c --
         # добавлены Фазой 5/6 (kernel/userdisk.c, kernel/bootconfirm.c): экран входа
         # теперь сам, внутри ядра, читает GPT (ищет раздел user-images, lib/gpt.c
         # зависит от lib/crc32.c) и заголовки зашифрованных образов (lib/userimg.c,
         # зависит от lib/crypto_rng.c/crypto_pbkdf2.c -- crypto_rng.c ядру
         # нужен и сам по себе, для одноразовой заглушки-пароля дисковых
         # пользователей в таблице сеанса, см. kernel/users.c
         # make_disk_backed_placeholder()); lib/bootmeta.c -- Фаза 6,
         # kernel/bootconfirm.c читает/пишет boot-meta через bootmeta_parse()/
         # bootmeta_serialize()/bootmeta_mark_confirmed() -- раньше это делали
         # только userspace-инструменты (apps/install-os.c/create-user.c/
         # atomictest.c) через syscall'ы. lib/bootmeta.c НЕ добавлен -- ядро им пока не
         # пользуется (это Фаза 6, stage2, который к тому же читает его
         # ДО какого-либо C-кода ядра вообще), добавлять раньше реальной
         # надобности не в духе этого дерева (см. соседние комментарии
         # выше про тот же принцип).
         # shell/shell.c -- убран отсюда Фазой 5.5 (см. CONTAINER_ARCHITECTURE.md):
         # шелл теперь обычное приложение, apps/shell.c, собираемое ниже вместе
         # со всеми остальными rootfs/bin/* через APP_SRCS, а не линкуемое в
         # kernel.bin. Файл shell/shell.c и include/shell.h удалены из дерева.
SRCS_S = boot/boot.s kernel/isr.s kernel/context.s kernel/smp_isr.s kernel/ap_trampoline_blob.s

# ВАЖНО: файлы .c и .s с одинаковым именем (kernel/isr.c и kernel/isr.s)
# иначе оба лягут в один и тот же kernel/isr.o -- компоновщик молча
# использует только один из них (тот, чьё неявное правило совпало первым),
# а другой файл никогда не попадёт в сборку. Поэтому объектники ассемблера
# получают отдельный суффикс .s.o, не пересекающийся с .c.o.
OBJS = $(SRCS_C:.c=.o) $(SRCS_S:.s=.s.o)

# ---- Трамплин запуска AP (SMP) ----
# boot/ap_trampoline.s -- отдельный от остального ядра плоский бинарник
# (16-битный код реального режима на входе, см. общий комментарий в
# самом файле), собираемый `nasm -f bin`, а не `-f elf64`, как всё
# остальное. kernel/ap_trampoline_blob.s (обычный elf64-файл, уже в
# SRCS_S выше) встраивает получившиеся байты в образ ядра через incbin
# -- эта строка гарантирует, что build/ap_trampoline.bin будет собран
# РАНЬШЕ, чем ассемблер дойдёт до incbin внутри ap_trampoline_blob.s.
build/ap_trampoline.bin: boot/ap_trampoline.s | build
	$(AS) -f bin boot/ap_trampoline.s -o build/ap_trampoline.bin

kernel/ap_trampoline_blob.s.o: build/ap_trampoline.bin

KERNEL = build/kernel.bin

# ---- Пользовательские бинарники (apps/*.c) ----
# Каждый собирается ОТДЕЛЬНО от ядра: со своим набором объектников
# (свой код + lib/string.c, но без остального ядра), своим линковочным
# скриптом apps/user.ld (фиксированный адрес загрузки, см.
# include/loader.h), и превращается в "плоский" бинарник через objcopy.
# Результат кладётся прямо в rootfs/bin/<name>, откуда его подхватывает
# gen_rootfs.py (см. цель `rootfs` ниже).
OBJCOPY = objcopy
APP_SRCS = $(wildcard apps/*.c)
APP_NAMES = $(notdir $(APP_SRCS:.c=))
APP_BINS = $(addprefix rootfs/bin/,$(APP_NAMES))

build/apps:
	mkdir -p build/apps

build/apps/lib_string.o: lib/string.c | build/apps
	$(CC) $(CFLAGS) -c lib/string.c -o $@

build/apps/lib_usyscall.o: lib/usyscall.c include/usyscall.h include/sysabi.h | build/apps
	$(CC) $(CFLAGS) -c lib/usyscall.c -o $@

build/apps/lib_libc.o: lib/libc.c include/libc.h | build/apps
	$(CC) $(CFLAGS) -c lib/libc.c -o $@

build/apps/%.o: apps/%.c include/kapi.h include/mlapp.h include/libc.h | build/apps
	$(CC) $(CFLAGS) -c $< -o $@

build/apps/%.elf: build/apps/%.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o

# ---- lib/mldb.c -- однофайловая БД (include/mldb.h) ----
# Отдельный объектник, слинкованный ТОЛЬКО в dbdemo.elf (явные правила
# ниже переопределяют generic-правило build/apps/%.elf выше именно для
# dbdemo -- GNU Make предпочитает explicit rule над pattern rule), чтобы
# не раздувать остальные бинарники (hello, fstest и т.д.) неиспользуемым
# ими кодом библиотеки.
build/apps/lib_mldb.o: lib/mldb.c include/mldb.h include/libc.h | build/apps
	$(CC) $(CFLAGS) -c lib/mldb.c -o $@

# dbdemo держит в памяти целую базу (все таблицы+строки) и дополнительно
# читает файл базы целиком во временный буфер при loaddb (см.
# apps/dbdemo.c, mldb_load()) -- стандартных 256 KiB частной кучи
# (LIBC_HEAP_SIZE в include/libc.h) маловато. Сама арена кучи -- это
# статический массив g_heap_arena внутри lib/libc.c (lib/libc.c, а НЕ
# apps/dbdemo.c!), поэтому увеличить её размер можно только пересобрав
# именно libc.c с другим -DLIBC_HEAP_SIZE -- собственный #define в
# dbdemo.c на это уже не повлияет, т.к. арена лежит в другой единице
# трансляции. Отсюда отдельный объектник dbdemo_libc.o вместо общего
# lib_libc.o, используемого остальными бинарниками.
build/apps/dbdemo_libc.o: lib/libc.c include/libc.h | build/apps
	$(CC) $(CFLAGS) -DLIBC_HEAP_SIZE=786432 -c lib/libc.c -o $@

build/apps/dbdemo.elf: build/apps/dbdemo.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/dbdemo_libc.o build/apps/lib_mldb.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/dbdemo_libc.o build/apps/lib_mldb.o

rootfs/bin/%: build/apps/%.elf
	mkdir -p rootfs/bin
	cp $< $@

# ---- lib/mbr.c -- сборка/разбор MBR в памяти (INSTALLER_PLAN.md, Фаза 1) ----
# Как и lib_mldb.o выше, отдельный объектник -- нужен только mbrtest.elf
# (и, позже, install-os.elf), не остальным приложениям.
build/apps/lib_mbr.o: lib/mbr.c include/mbr.h | build/apps
	$(CC) $(CFLAGS) -c lib/mbr.c -o $@

build/apps/mbrtest.elf: build/apps/mbrtest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_mbr.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_mbr.o

# ---- lib/crypto_sha256.c + lib/crypto_rng.c -- крипто-примитивы (ATOMIC_PLAN.md, Фаза 1) ----
# Как и lib_mbr.o/lib_gpt.o выше -- отдельные объектники, переиспользуемые
# несколькими бинарниками. lib_crypto_hmac.o/lib_crypto_pbkdf2.o/
# lib_crypto_aes.o добавлены при написании Фазы 3 (include/userimg.h) --
# см. общий комментарий там же за тем, почему они логически тоже
# "Фаза 1" (примитивы), хотя появились позже по времени.
build/apps/lib_crypto_sha256.o: lib/crypto_sha256.c include/crypto_sha256.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_sha256.c -o $@

build/apps/lib_crypto_rng.o: lib/crypto_rng.c include/crypto_rng.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_rng.c -o $@

build/apps/lib_crypto_hmac.o: lib/crypto_hmac.c include/crypto_hmac.h include/crypto_sha256.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_hmac.c -o $@

build/apps/lib_crypto_pbkdf2.o: lib/crypto_pbkdf2.c include/crypto_pbkdf2.h include/crypto_hmac.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_pbkdf2.c -o $@

build/apps/lib_crypto_aes.o: lib/crypto_aes.c include/crypto_aes.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_aes.c -o $@

build/apps/lib_crypto_xts.o: lib/crypto_xts.c include/crypto_xts.h include/crypto_aes.h include/crypto_hmac.h | build/apps
	$(CC) $(CFLAGS) -c lib/crypto_xts.c -o $@

# cryptoblk.c -- ATOMIC_PLAN.md, Фаза 4: прозрачный AES-XTS слой поверх
# абстрактного посекторного I/O (см. общий комментарий в
# include/cryptoblk.h). cryptoblktest.elf -- round-trip проверка на
# поддельном диске в памяти, тем же духом, что atomictest.elf (Фаза 2)
# и usertest.elf (Фаза 3) -- ни разу не трогает настоящий диск.
build/apps/lib_cryptoblk.o: lib/cryptoblk.c include/cryptoblk.h include/crypto_xts.h | build/apps
	$(CC) $(CFLAGS) -c lib/cryptoblk.c -o $@

build/apps/cryptoblktest.elf: build/apps/cryptoblktest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_cryptoblk.o build/apps/lib_crypto_xts.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_cryptoblk.o build/apps/lib_crypto_xts.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o

build/apps/cryptotest.elf: build/apps/cryptotest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_aes.o build/apps/lib_crypto_xts.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_aes.o build/apps/lib_crypto_xts.o

# ---- lib/bootmeta.c -- формат сектора boot-meta (ATOMIC_PLAN.md, Фаза 2) ----
build/apps/lib_bootmeta.o: lib/bootmeta.c include/bootmeta.h | build/apps
	$(CC) $(CFLAGS) -c lib/bootmeta.c -o $@

# lib_gpt.o уже собирается ниже (используется gpttest.elf) -- переиспользуем.
build/apps/atomictest.elf: build/apps/atomictest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o build/apps/lib_crypto_rng.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o build/apps/lib_crypto_rng.o

# ---- lib/gpt.c + lib/crc32.c -- сборка/разбор GPT в памяти (UEFI_PLAN.md, Фаза A) ----
build/apps/lib_crc32.o: lib/crc32.c include/crc32.h | build/apps
	$(CC) $(CFLAGS) -c lib/crc32.c -o $@

build/apps/lib_gpt.o: lib/gpt.c include/gpt.h include/crc32.h | build/apps
	$(CC) $(CFLAGS) -c lib/gpt.c -o $@

build/apps/gpttest.elf: build/apps/gpttest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o

# install-os -- та же lib_mbr.o, что и mbrtest (mbr_build() из Фазы 1),
# плюс обычные lib_string/lib_usyscall/lib_libc, плюс (ATOMIC_PLAN.md,
# Фаза 2, хвост) lib_gpt.o/lib_crc32.o/lib_bootmeta.o/lib_crypto_rng.o --
# install-os теперь единственный бинарник, реально пишущий атомарную
# GPT-разметку на диск (CAP_DISK_RAW даётся только ему, см. общий
# комментарий у install_atomic_layout() в apps/install-os.c).
build/apps/install-os.elf: build/apps/install-os.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_mbr.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o build/apps/lib_crypto_rng.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_mbr.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o build/apps/lib_crypto_rng.o

# update-os -- ATOMIC_PLAN.md, Фаза 7 -- та же связка lib_gpt.o/lib_crc32.o
# (найти boot-meta/os-slot-a/os-slot-b на уже установленном диске) +
# lib_bootmeta.o (bootmeta_parse()/bootmeta_serialize()/bootmeta_other_slot()),
# что install-os/atomictest выше -- lib_crypto_rng.o НЕ нужен (в отличие
# от install-os -- update-os не строит GPT с нуля, gpt_build() сюда не
# линкуется вовсе, а значит и её зависимость lib_crypto_rng.o (disk_guid)
# тоже не нужна).
build/apps/update-os.elf: build/apps/update-os.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_gpt.o build/apps/lib_crc32.o build/apps/lib_bootmeta.o

apps: $(APP_BINS)

# ---- lib/userimg.c -- заголовок зашифрованного пользовательского
# образа (ATOMIC_PLAN.md, Фаза 3) -- см. общий комментарий в
# include/userimg.h. usertest.elf -- round-trip проверка в памяти, тем
# же духом, что atomictest.elf для Фазы 2 (см. общий комментарий в
# apps/usertest.c за тем, почему apps/create-user.c, который реально
# писал бы слот на диск, здесь ещё не появляется).
build/apps/lib_userimg.o: lib/userimg.c include/userimg.h include/crypto_rng.h include/crypto_pbkdf2.h include/crypto_hmac.h include/crypto_aes.h | build/apps
	$(CC) $(CFLAGS) -c lib/userimg.c -o $@

build/apps/usertest.elf: build/apps/usertest.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_userimg.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_userimg.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o

# create-user -- ATOMIC_PLAN.md, Фаза 3: реальная запись слота на диск,
# нуждается в lib_gpt.o/lib_crc32.o (найти раздел user-images) поверх
# того же набора, что usertest.elf. disk_raw -- см. общий комментарий у
# CAP_DISK_RAW (include/container.h) и rootfs/containers/create-user.manifest.
build/apps/create-user.elf: build/apps/create-user.o build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_userimg.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o build/apps/lib_gpt.o build/apps/lib_crc32.o apps/user.ld
	$(LD) -m elf_x86_64 -T apps/user.ld -nostdlib -z max-page-size=0x1000 \
	      -o $@ $< build/apps/lib_string.o build/apps/lib_usyscall.o build/apps/lib_libc.o build/apps/lib_userimg.o build/apps/lib_crypto_rng.o build/apps/lib_crypto_pbkdf2.o build/apps/lib_crypto_hmac.o build/apps/lib_crypto_sha256.o build/apps/lib_crypto_aes.o build/apps/lib_gpt.o build/apps/lib_crc32.o

all: build/test_boot.img

build:
	mkdir -p build

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.s.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): build $(OBJS)
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)

# ---- kernel.flat + собственные stage1/stage2 (GRUB из проекта вырезан) ----
# kernel.flat -- голый дамп байт ELF-сегментов kernel.bin, готовых лечь
# ровно по адресу 1M (см. ". = 1M;" в boot/linker.ld). GRUB умел бы
# разбирать сам ELF, наш stage2 -- нет и не должен: ему нужен именно
# такой "сырой" образ.
build/kernel.flat: $(KERNEL)
	$(OBJCOPY) -O binary $(KERNEL) build/kernel.flat

build/stage1.bin: boot/stage1.s | build
	nasm -f bin boot/stage1.s -o build/stage1.bin

build/stage2.bin: boot/stage2.s | build
	nasm -f bin boot/stage2.s -o build/stage2.bin

# ---- ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий в начале
# boot/stage2_atomic.s за полной картиной. stage1_gpt.bin -- ТОТ ЖЕ
# исходник, что stage1.bin (boot/stage1.s), собранный с другим стартовым
# LBA под stage2 (-D STAGE2_LBA_START=34 -- см. общий комментарий в
# boot/stage1.s за %ifndef -- 34 = ATOMIC_GPT_RESERVED_SECTORS, сразу
# после первичной GPT-таблицы, см. также apps/install-os.c) -- ПОЭТОМУ
# у него нет отдельного .s файла, только другая команда сборки того же
# boot/stage1.s. stage2_atomic.bin -- отдельный исходник
# (boot/stage2_atomic.s), собирается как обычно.
build/stage1_gpt.bin: boot/stage1.s | build
	nasm -f bin -D STAGE2_LBA_START=34 boot/stage1.s -o build/stage1_gpt.bin

build/stage2_atomic.bin: boot/stage2_atomic.s | build
	nasm -f bin boot/stage2_atomic.s -o build/stage2_atomic.bin

# Сырой диск-образ для загрузки в QEMU без единого байта GRUB/ISO:
# `qemu-system-x86_64 -drive file=build/test_boot.img,format=raw` --
# см. NOGRUB_PLAN.md, раздел "Порядок тестирования". Разметка секторов:
#   LBA 0        -- stage1.bin (MBR, 512 байт)
#   LBA 1..63    -- stage2.bin (63 сектора = 31.5 KiB, с большим запасом)
#   LBA 64..32831 -- kernel.flat, дополненный нулями до ровно 32768 секторов
#                   (16 MiB, поднято с 2, 4, затем 8 MiB -- см. ту же константу в
#                   boot/stage2.s/apps/install-os.c) -- фиксированный
#                   размер "области ядра"
# FAT32/UFS-раздел (LBA 4160+) сюда сознательно не входит -- этот образ
# только для загрузки, реальная установка на диск с разделом данных --
# через install-os (см. INSTALLER_PLAN.md, Фаза 3+4). ЭТОТ образ --
# ВСЕГДА legacy (NOGRUB) разметка, не Atomic -- см. ATOMIC_PLAN.md,
# Фаза 6, раздел "Как проверить живьём в QEMU" за тем, как собрать и
# проверить Atomic-образ (через install-os, а не этот скрипт).
build/test_boot.img: build/stage1.bin build/stage2.bin build/kernel.flat
	python3 tools/make_test_boot_img.py build/test_boot.img build/stage1.bin build/stage2.bin build/kernel.flat

clean:
	rm -f $(OBJS)
	rm -rf build

# Пересобирает fs/ramfs_data.c и include/ramfs_data.h из содержимого rootfs/.
# Запускать вручную после изменения файлов в rootfs/ (например, обновления
# readme.txt) или после изменения исходников в apps/ (rootfs зависит от
# apps, поэтому бинарники в rootfs/bin/ всегда пересобираются заново перед
# упаковкой в ramfs_data.c).
rootfs: apps
	python3 tools/gen_rootfs.py

# disk0.img -- диск на старом IDE-контроллере (как раньше, для FAT32-теста).
# disk1.img -- диск на AHCI-контроллере (SATA), появившемся в этой версии;
# создайте его заранее, например: qemu-img create -f raw disk1.img 64M
#
# -usb обязателен: без него PIIX3 не выставляет UHCI-контроллер вообще,
# и usb_init() в kernel.c просто ничего не найдёт (это не ошибка, см.
# drivers/usb_uhci.c). usb-kbd/usb-mouse -- виртуальные HID-устройства
# для проверки usb_hid.c без реального оборудования. Чтобы проверить
# usb_msd.c (флеш-накопитель), создайте disk2.img (qemu-img create -f
# raw disk2.img 64M) и добавьте:
#   -drive if=none,id=usbstick,format=raw,file=disk2.img -device usb-storage,drive=usbstick
#
# `run` грузится через собственный stage1/stage2 (без GRUB и без ISO):
# build/test_boot.img подаётся обычным `-drive ...,format=raw`, ровно
# так же, как видела бы это реальная машина/флешка.
run: build/test_boot.img
	qemu-system-x86_64 -m 4096 -smp 4 -drive file=build/test_boot.img,format=raw -enable-kvm \
	    -usb -device usb-kbd -device usb-mouse

.PHONY: all clean run rootfs apps

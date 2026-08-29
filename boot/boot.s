; boot.s -- Multiboot header + переход в 64-битный Long Mode
; Собирается через nasm в формат elf64. GRUB запускает нас в 32-битном
; protected mode независимо от битности ядра, поэтому первая часть файла
; написана как 32-битный код, который готовит paging и переключается
; в 64-битный режим, а вторая часть -- уже настоящий 64-битный код.

MBALIGN     equ  1<<0
MEMINFO     equ  1<<1
FLAGS       equ  MBALIGN | MEMINFO
MAGIC       equ  0x1BADB002
CHECKSUM    equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
; Четыре таблицы PD (Page Directory), по одной на каждый GiB нижних 4 GiB
; физического адресного пространства. Раньше (пока не было PCI-устройств
; с MMIO BAR) identity-map покрывал только первый 1 GiB -- этого хватало
; для ядра и ОЗУ, но BAR такого контроллера, как AHCI, обычно выделяется
; QEMU/BIOS где-то в "PCI hole" ближе к 4 GiB, то есть заметно выше 1 GiB.
; Отображаем все 4 GiB, чтобы физический адрес из PCI BAR был доступен
; напрямую (identity map: физический адрес == виртуальный адрес).
p2_table_0:
    resb 4096
p2_table_1:
    resb 4096
p2_table_2:
    resb 4096
p2_table_3:
    resb 4096
align 16
stack_bottom:
    resb 65536              ; 64 KiB стек ядра
stack_top:

; ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у _start_native выше:
; ecx нужно сохранить куда-то помимо регистра на время
; check_cpuid/check_long_mode/setup_page_tables. Физический адрес этой
; ячейки идентично отображён что до, что после enable_paging (identity
; map всех нижних 4 GiB, см. setup_page_tables) -- тот же приём, каким
; уже пользуются p4_table и остальные таблицы страниц здесь же.
align 4
saved_atomic_info:
    resd 1

section .rodata
align 4
gdt64:
    dq 0                                      ; нулевой дескриптор
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)  ; code, present, long mode (L=1)
.data: equ $ - gdt64
    dq (1<<44) | (1<<47) | (1<<41)            ; data, present, writable
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

msg_no_cpuid    db "ERROR: CPUID not supported", 0
msg_no_longmode db "ERROR: CPU does not support 64-bit long mode", 0

; ---- Нативная (без GRUB) точка входа -- см. NOGRUB_PLAN.md, Фаза B ----
; Зеркало _start ниже, но для собственного stage1/stage2 вместо GRUB:
; boot/stage2.s (32-битный protected mode, после того как сам прочитал
; kernel.flat с диска через ATA PIO) прыгает СЮДА -- на ФИКСИРОВАННЫЙ
; адрес KERNEL_LOAD_ADDR + 0x1000 (1M + 4K), не зная и не умея узнать
; никакой символ ELF (у него на руках только сырые байты kernel.flat,
; без таблицы символов). Этот фиксированный адрес гарантирован линковщиком
; (см. boot/linker.ld: .text.entry -- всегда первая внутри 4K-выровненной
; .text, которая сама всегда начинается сразу после 12-байтного
; .multiboot, тоже выровненного на 4K) -- поэтому единственное жёсткое
; требование к этому файлу: _start_native должен быть ПЕРВЫМ символом
; здесь, ничего не должно попасть впереди него в секцию .text.entry.
;
; Соглашение о регистрах (симметрично _start/magic/mb_info ниже, только
; здесь передаётся не Multiboot-специфичный mb_info, а сырой E820-буфер --
; см. boot/stage2.s: get_e820 и kernel/bootinfo_e820.c):
;   eax = физический адрес буфера сырых E820-записей (boot/stage2.s: E820_BUFFER)
;   ebx = число записей в нём
;   ecx = ATOMIC_PLAN.md, Фаза 6 -- 0, если загрузчик НЕ Atomic-осведомлён
;         (boot/stage2.s, обычный NOGRUB-путь -- см. там xor ecx, ecx перед
;         этим же прыжком), иначе физический адрес блока
;         ATOMIC_BOOTINFO_MAGIC (boot/stage2_atomic.s, см. общий
;         комментарий у этой константы, include/bootinfo.h). Сохраняется
;         в saved_atomic_info НИЖЕ, а не в регистре: check_cpuid/
;         check_long_mode (через cpuid) и setup_page_tables (через
;         fill_pd_*, которым нужны eax/ebx/ecx/edx/ebp) все вместе
;         занимают весь набор регистров общего назначения хотя бы один
;         раз -- ни один из них не остаётся гарантированно свободным
;         через всю цепочку вызовов, в отличие от edi/esi (см. комментарий
;         ниже, "ВАЖНО").
section .text.entry
bits 32
global _start_native
extern bootinfo_from_e820

_start_native:
    cli
    mov esp, stack_top
    mov [saved_atomic_info], ecx  ; см. комментарий выше про регистры -- сохраняем
                                    ; до setup_page_tables/check_long_mode, они его не пощадят
    mov edi, eax              ; сохраняем e820-буфер (не трогается cpuid)
    mov esi, ebx               ; сохраняем число записей

    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging
    call enable_sse

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start_native

section .text
bits 32
global _start
extern kernel_main
extern bootinfo_from_multiboot

_start:
    cli
    mov esp, stack_top
    mov edi, ebx             ; сохраняем указатель на multiboot info (не трогается cpuid)
    mov esi, eax             ; сохраняем magic number

    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging
    call enable_sse

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

; ---- Проверка наличия CPUID (пытаемся переключить бит ID в EFLAGS) ----
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov esi, msg_no_cpuid
    jmp print_error32

; ---- Проверка поддержки Long Mode через расширенный CPUID ----
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov esi, msg_no_longmode
    jmp print_error32

; ---- Печать сообщения об ошибке напрямую в текстовый буфер VGA (0xB8000) ----
; вход: esi = указатель на ASCIIZ-строку
print_error32:
    mov edi, 0xB8000
    mov ah, 0x4F              ; белый текст на красном фоне
.loop:
    lodsb
    test al, al
    jz .done
    mov [edi], al
    mov [edi+1], ah
    add edi, 2
    jmp .loop
.done:
    cli
.hang:
    hlt
    jmp .hang

; ---- Строим таблицы страниц: identity-map нижних 4 GiB ----
; PML4[0] -> PDPT[0..3] -> для каждого PDPT-элемента своя PD[0..511],
; используя 2 MiB huge pages. Итого 4 * 512 * 2 MiB = 4 GiB.
;
; Верхний GiB (3-й, 0xC0000000-0xFFFFFFFF) на типичных платформах QEMU --
; это "PCI hole", куда попадают MMIO BAR PCI-устройств (например AHCI).
; Для него дополнительно ставим бит PCD (Page Cache Disable, бит 4) --
; кэшировать регистры устройства нельзя, иначе драйвер будет читать
; устаревшие значения из кэша вместо реального состояния железа.
; Нижние 3 GiB (ОЗУ и образ ядра) остаются обычными кэшируемыми страницами.
setup_page_tables:
    mov eax, p3_table
    or eax, 0x03                ; present, writable
    mov [p4_table], eax

    mov eax, p2_table_0
    or eax, 0x03
    mov [p3_table], eax
    mov eax, p2_table_1
    or eax, 0x03
    mov [p3_table + 8], eax
    mov eax, p2_table_2
    or eax, 0x03
    mov [p3_table + 16], eax
    mov eax, p2_table_3
    or eax, 0x03
    mov [p3_table + 24], eax

    call fill_pd_0
    call fill_pd_1
    call fill_pd_2
    call fill_pd_3
    ret

; ВАЖНО: ни одна из процедур ниже не трогает edi/esi -- в них на момент
; этого вызова (см. _start) сохранены mb_info и magic, которые нужны
; чуть позже, в long_mode_start. Используем только eax/ebx/ecx/edx/ebp.
;
; Каждая из четырёх процедур заполняет свою PD одинаковым способом
; (см. fill_pd_common), отличаясь только базовым физическим адресом,
; целевой таблицей и флагами страницы (передаются как immediate, а не
; через регистр, чтобы не тратить на это ещё один "занятый" регистр).
fill_pd_0:
    mov ebx, p2_table_0
    xor ebp, ebp                 ; базовый адрес этой PD = 0 * 1 GiB
    call fill_pd_common_cached
    ret
fill_pd_1:
    mov ebx, p2_table_1
    mov ebp, 0x40000000          ; 1 GiB
    call fill_pd_common_cached
    ret
fill_pd_2:
    mov ebx, p2_table_2
    mov ebp, 0x80000000          ; 2 GiB
    call fill_pd_common_cached
    ret
fill_pd_3:
    mov ebx, p2_table_3
    mov ebp, 0xC0000000          ; 3 GiB -- "PCI hole" на QEMU
    call fill_pd_common_uncached
    ret

; вход: ebx = адрес PD-таблицы, ebp = базовый физический адрес (кратен 1 GiB)
; флаги: present, writable, PS=1 (обычные кэшируемые 2 MiB страницы)
fill_pd_common_cached:
    xor ecx, ecx
.loop:
    mov eax, 0x200000
    mul ecx
    add eax, ebp
    or eax, 0x83
    mov [ebx + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

; то же самое, но с флагом PCD=1 (Page Cache Disable) -- для региона,
; где ожидаются MMIO-регистры устройств, которые нельзя кэшировать.
fill_pd_common_uncached:
    xor ecx, ecx
.loop:
    mov eax, 0x200000
    mul ecx
    add eax, ebp
    or eax, 0x93
    mov [ebx + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5               ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    or eax, 1 << 8                ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31               ; CR0.PG
    mov cr0, eax
    ret

; ---- Включаем SSE: double в System V AMD64 ABI возвращается через xmm0 ----
enable_sse:
    mov eax, cr0
    and ax, 0xFFFB        ; очищаем CR0.EM (бит 2)
    or ax, 0x2            ; ставим CR0.MP (бит 1)
    mov cr0, eax

    mov eax, cr4
    or eax, 3 << 9         ; CR4.OSFXSR (бит 9) и CR4.OSXMMEXCPT (бит 10)
    mov cr4, eax
    ret

bits 64
long_mode_start:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top

    ; До сих пор: edi = mb_info, esi = magic (32-битные значения, верхние
    ; биты обнулены, т.к. пишем в них уже находясь в 64-битном режиме).
    ; System V AMD64 ABI требует: rdi = 1-й аргумент, rsi = 2-й аргумент.
    ; bootinfo_from_multiboot(magic, mb_info) -> нужно поменять местами
    ; (см. общий комментарий в kernel/bootinfo_multiboot.c за тем, зачем
    ; вообще нужен этот промежуточный вызов -- NOGRUB_PLAN.md, Фаза A:
    ; kernel_main() ниже больше не получает magic/mb_info напрямую и
    ; ничего не знает про Multiboot, только про boot_info_t).
    mov eax, edi                  ; eax = mb_info
    mov edi, esi                  ; rdi = magic          (1-й аргумент)
    mov esi, eax                  ; rsi = mb_info        (2-й аргумент)

    call bootinfo_from_multiboot  ; rax = const boot_info_t*

    mov rdi, rax                  ; kernel_main(bi) -- ОДИН аргумент
    call kernel_main

.hang64:
    cli
    hlt
    jmp .hang64

; ---- Нативная (без GRUB) точка входа в 64-битном коде -- см. общий
; комментарий в _start_native выше (section .text.entry) и в
; kernel/bootinfo_e820.c. Отличается от long_mode_start ровно одной
; строчкой: вызывает bootinfo_from_e820() вместо bootinfo_from_multiboot()
; -- вся остальная работа (сегменты, стек, вызов kernel_main) идентична,
; т.к. к этому моменту оба пути уже неотличимы: CPU в Long Mode с paging,
; единственная разница -- откуда взялся boot_info_t. ----
extern bootinfo_from_e820
global long_mode_start_native
long_mode_start_native:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top

    ; edi = e820-буфер, esi = число записей (см. _start_native выше) --
    ; уже ровно в порядке System V AMD64 ABI (rdi = 1-й аргумент,
    ; rsi = 2-й), менять местами, в отличие от long_mode_start, не нужно.
    ; rdx = 3-й аргумент -- см. ATOMIC_PLAN.md, Фаза 6: saved_atomic_info,
    ; отложенный в _start_native (см. общий комментарий там же).
    mov edx, [saved_atomic_info]
    call bootinfo_from_e820       ; rax = const boot_info_t*

    mov rdi, rax
    call kernel_main

.hang64_native:
    cli
    hlt
    jmp .hang64_native

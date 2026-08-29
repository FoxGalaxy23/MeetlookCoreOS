; stage2.s -- см. NOGRUB_PLAN.md, Фаза B. Грузится stage1'ом по адресу
; 0x8000 (реальный режим). Задачи по порядку:
;   1) собрать карту физической памяти (BIOS INT 15h, EAX=0xE820) --
;      единственное, что нужно делать именно в реальном режиме, пока
;      BIOS ещё доступен;
;   2) включить линию A20 (без неё адреса выше 1 MiB "заворачиваются");
;   3) переключиться в 32-битный protected mode (свой минимальный GDT);
;   4) прочитать "область ядра" (см. разметку диска в NOGRUB_PLAN.md --
;      фиксированные LBA 64..4159, 4096 секторов = 2 MiB) напрямую в
;      память по адресу 1 MiB через собственный минимальный ATA PIO
;      driver -- в protected mode BIOS (INT 13h) уже недоступен, поэтому
;      это не тот же путь, что использовал stage1;
;   5) передать управление в _start_native (boot/boot.s) -- ТУ ЖЕ самую
;      точку входа, которую использует kernel.flat независимо от того,
;      GRUB его загрузил или мы сами (см. общий комментарий в boot.s
;      про NATIVE_ENTRY_OFFSET).
;
; ДИАГНОСТИКА. Все контрольные точки печатаются в COM1 (0x3F8) сырым
; выводом через порт, без каких-либо структур данных -- работает
; идентично в 16- и 32-битном режиме (IN/OUT всегда используют DX как
; 16-битный номер порта независимо от битности кода). В QEMU это видно
; напрямую в терминале через `-serial stdio` -- НЕ то же самое, что VGA
; text mode (который правки эти пока не трогают, туда пишет уже сам
; kernel.bin через drivers/vga.c). Пригодится и после сборки install-os
; (Фаза C, INSTALLER_PLAN.md) -- диагностика самого низкоуровневого
; этапа загрузки без VGA/видео вообще, полезно и для будущих ARM/RISC-V
; портов, где консоль тоже почти всегда serial, а не текстовый видеобуфер.

bits 16
org 0x8000

; ---- Общие константы (используются и в 16-, и в 32-битной части) ----
E820_BUFFER      equ 0x9000    ; сырые 20-байтные записи BIOS E820 подряд
E820_MAX_ENTRIES equ 64         ; 64*20 = 1280 байт, буфер 0x9000..0x94FF -- с большим запасом
KERNEL_LOAD_ADDR equ 0x00100000 ; 1 MiB -- тот же адрес, что и ". = 1M" в boot/linker.ld
KERNEL_LBA_START equ 64         ; см. разметку диска, NOGRUB_PLAN.md
KERNEL_SECTORS   equ 32768      ; 16 MiB / 512 -- фиксированный размер "области ядра"
                                  ; (поднято с 2 MiB, затем с 4 MiB -- kernel.flat перерос старый
                                  ; бюджет после ATOMIC_PLAN.md Фаз 1-6, см. общий
                                  ; комментарий в apps/install-os.c у той же константы)
NATIVE_ENTRY_OFF equ 0x1000     ; см. общий комментарий в boot/boot.s про .text.entry

start:
    ; DS/ES/SS уже 0 после stage1 (не трогаем), но SP стоило бы поставить
    ; заново -- stage1's 0x7C00 всё ещё безопасен (сам stage1 больше не
    ; исполняется, ничего важного там не осталось).
    mov sp, 0x7C00

    call init_serial16
    mov si, msg_stage2_start
    call serial_print16

    call get_e820
    mov si, msg_e820_done
    call serial_print16

    call enable_a20
    mov si, msg_a20_done
    call serial_print16

    cli
    lgdt [gdt32.pointer]
    mov eax, cr0
    or eax, 1                 ; CR0.PE
    mov cr0, eax
    jmp gdt32.code:pm_start   ; дальний jmp -- одновременно и переключает
                               ; CS на 32-битный селектор, и сбрасывает
                               ; конвейер декодирования инструкций

; ---------------------------------------------------------------------
; ---- BIOS INT 15h, EAX=0xE820 -- карта физической памяти ----
; Стандартный, хорошо документированный алгоритм: продолжаем, пока BIOS
; возвращает CF=0 и "SMAP" в EAX, используя EBX как курсор состояния
; (BIOS сам поддерживает, чем именно он его наполняет -- мы просто
; передаём его же значение на следующий вызов, не глядя внутрь).
; ---------------------------------------------------------------------
get_e820:
    push es
    push di
    push ebx

    xor ax, ax
    mov es, ax
    mov di, E820_BUFFER
    xor ebx, ebx
    mov word [e820_count], 0

.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150        ; "SMAP"
    mov ecx, 20
    int 0x15
    jc .done                    ; CF=1 -- конец списка (или BIOS не поддерживает)
    cmp eax, 0x534D4150
    jne .done                   ; защита от BIOS, вернувшего мусор

    inc word [e820_count]
    add di, 20

    cmp word [e820_count], E820_MAX_ENTRIES
    jae .done

    test ebx, ebx
    jz .done                    ; ebx=0 после вызова -- это была последняя запись
    jmp .loop

.done:
    pop ebx
    pop di
    pop es
    ret

; ---------------------------------------------------------------------
; ---- A20 -- "быстрый" метод через порт 0x92 ----
; Достаточно и надёжно для QEMU (наша основная тестовая платформа, см.
; INSTALLER_PLAN.md/NOGRUB_PLAN.md); на части реального железа этот
; конкретный метод не срабатывает и нужен запасной (через контроллер
; клавиатуры, 8042) -- сознательно оставлено вне MVP (тот же принцип,
; что и "тестируем только в QEMU" во всём плане).
; ---------------------------------------------------------------------
enable_a20:
    in al, 0x92
    test al, 2
    jnz .done                   ; уже включена (некоторые BIOS делают это сами)
    or al, 2
    and al, 0xFE                ; на всякий случай не дёргаем fast-reset бит (bit 0)
    out 0x92, al
.done:
    ret

; ---------------------------------------------------------------------
; ---- UART (COM1, 0x3F8) -- минимальная инициализация 16550 ----
; 38400 8N1 -- ровно то, что ожидает QEMU `-serial stdio` без
; дополнительных флагов на другой стороне.
; ---------------------------------------------------------------------
init_serial16:
    mov dx, 0x3F9              ; IER -- отключаем прерывания UART (не нужны,
    xor al, al                  ; читаем/пишем только через polling)
    out dx, al
    mov dx, 0x3FB               ; LCR -- включаем DLAB, чтобы задать делитель
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8                ; делитель, младший байт (0x03 -> 38400 бод)
    mov al, 0x03
    out dx, al
    mov dx, 0x3F9                ; делитель, старший байт
    xor al, al
    out dx, al
    mov dx, 0x3FB                 ; LCR -- 8 бит, без чётности, 1 стоп-бит, DLAB=0
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA                  ; FCR -- включить FIFO, очистить, порог 14 байт
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC                  ; MCR -- DTR/RTS
    mov al, 0x0B
    out dx, al
    ret

; вход: al = байт для отправки (16-битный контекст)
serial_putc16:
    push dx
    push ax
.wait:
    mov dx, 0x3FD               ; LSR
    in al, dx
    test al, 0x20                ; THR empty?
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

; вход: si = указатель на ASCIIZ-строку (16-битный контекст)
serial_print16:
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putc16
    jmp .loop
.done:
    pop si
    pop ax
    ret

msg_stage2_start db "stage2: start", 13, 10, 0
msg_e820_done    db "stage2: e820 done", 13, 10, 0
msg_a20_done     db "stage2: a20 enabled, entering protected mode", 13, 10, 0
msg_kernel_done  db "stage2: kernel loaded, jumping to native entry", 13, 10, 0

e820_count: dw 0

; ---- Минимальный "плоский" GDT для 32-битного protected mode ----
align 8
gdt32:
    dq 0
.code: equ $ - gdt32
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00   ; code, 32-бит, лимит 4 GiB
.data: equ $ - gdt32
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00   ; data, 32-бит, лимит 4 GiB
.pointer:
    dw $ - gdt32 - 1
    dd gdt32

; =======================================================================
bits 32

pm_start:
    mov ax, gdt32.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00              ; тот же приём, что и в реальном режиме --
                                   ; растёт вниз от начала стека stage1,
                                   ; ничего важного там больше нет

    call serial_init_check32       ; см. общий комментарий ниже -- порт уже
                                    ; настроен из 16-битного кода, здесь
                                    ; просто продолжаем им пользоваться

    mov esi, msg_pm_start
    call serial_print32

    call load_kernel

    mov esi, msg_kernel_done
    call serial_print32

    ; ---- Передаём управление в _start_native (см. boot/boot.s) ----
    ; System V-подобное соглашение, зеркальное тому, что _start
    ; использует для magic/mb_info (см. общий комментарий в boot.s):
    ;   eax = физический адрес буфера сырых E820-записей
    ;   ebx = число записей в нём
    ;   ecx = 0 -- ATOMIC_PLAN.md, Фаза 6: этот (обычный, NOGRUB) stage2
    ;         НЕ Atomic-осведомлён, boot-meta не читает -- сигнализируем
    ;         boot.s/bootinfo_from_e820(), что atomic_info_ptr отсутствует
    ;         (см. общий комментарий у ATOMIC_BOOTINFO_MAGIC,
    ;         include/bootinfo.h, и у _start_native, boot/boot.s) -- БЕЗ
    ;         этого регистр нёс бы неопределённый мусор с предыдущих
    ;         вычислений (ecx использовался как счётчик в load_kernel), и
    ;         bootinfo_from_e820() могла бы случайно принять его за
    ;         настоящий указатель.
    mov eax, E820_BUFFER
    movzx ebx, word [e820_count]
    xor ecx, ecx
    jmp KERNEL_LOAD_ADDR + NATIVE_ENTRY_OFF

; Никакой реальной "проверки" тут нет -- просто сохраняем симметрию с
; init_serial16 по названию; UART уже настроен в реальном режиме, порт --
; это просто число (0x3F8), а не структура, инициализация не сбрасывается
; переходом в protected mode.
serial_init_check32:
    ret

; вход: al = байт для отправки (32-битный контекст -- те же порты,
; IN/OUT одинаково работают в 16- и 32-битном коде)
serial_putc32:
    push edx
    push eax
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    pop eax
    mov dx, 0x3F8
    out dx, al
    pop edx
    ret

; вход: esi = указатель на ASCIIZ-строку (32-битный контекст)
serial_print32:
    push eax
    push esi
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putc32
    jmp .loop
.done:
    pop esi
    pop eax
    ret

msg_pm_start db "stage2: protected mode entered, loading kernel", 13, 10, 0

; ---------------------------------------------------------------------
; ---- Минимальный ATA PIO (LBA28) driver -- ТОЛЬКО чтение, ТОЛЬКО
; первичный канал (0x1F0), ТОЛЬКО master. Тот же протокол (регистры,
; порядок команд, поллинг BSY/DRQ), что уже реализован в C для самой ОС
; в drivers/ide.c (ide_read_sectors()) -- см. общий комментарий там же;
; здесь -- его вынужденный дубль, т.к. в этой точке загрузки настоящего
; драйвера/прерываний/кучи ещё не существует, а BIOS (INT 13h) уже
; недоступен (мы в protected mode). Читает КОЛИЧЕСТВО секторов,
; кратное CHUNK_SECTORS, чанками -- т.к. регистр SECCOUNT0 -- 8-битный
; (максимум 255 за одну команду).
; ---------------------------------------------------------------------
ATA_IO        equ 0x1F0
ATA_DATA      equ ATA_IO + 0
ATA_SECCOUNT  equ ATA_IO + 2
ATA_LBA0      equ ATA_IO + 3
ATA_LBA1      equ ATA_IO + 4
ATA_LBA2      equ ATA_IO + 5
ATA_DRVSEL    equ ATA_IO + 6
ATA_CMD       equ ATA_IO + 7
ATA_STATUS    equ ATA_IO + 7
ATA_SR_BSY    equ 0x80
ATA_SR_DRQ    equ 0x08

CHUNK_SECTORS equ 128            ; < 256 -- влезает в 8-битный SECCOUNT0

; вход: ничего. Читает KERNEL_SECTORS секторов начиная с KERNEL_LBA_START
; в KERNEL_LOAD_ADDR. Портит eax/ebx/ecx/edx/esi/edi (вызывающий код --
; pm_start -- на момент вызова их уже не использует).
load_kernel:
    mov edi, KERNEL_LOAD_ADDR       ; edi = текущий адрес назначения (растёт)
    mov ebp, KERNEL_LBA_START        ; ebp = текущий LBA (растёт) -- ebp
                                       ; свободен, ни один из вызовов внутри
                                       ; цикла его не трогает
    mov esi, KERNEL_SECTORS / CHUNK_SECTORS  ; esi = число чанков, кратно ровно
                                               ; (4096 / 128 = 32 -- без остатка)

.chunk_loop:
    call ata_wait_bsy_clear

    mov eax, ebp
    shr eax, 24
    and al, 0x0F
    or al, 0xE0                     ; master, LBA-режим (см. drivers/ide.c: 0xE0 | ...)
    mov dx, ATA_DRVSEL
    out dx, al

    mov al, CHUNK_SECTORS
    mov dx, ATA_SECCOUNT
    out dx, al

    mov eax, ebp
    mov dx, ATA_LBA0
    out dx, al
    mov eax, ebp
    shr eax, 8
    mov dx, ATA_LBA1
    out dx, al
    mov eax, ebp
    shr eax, 16
    mov dx, ATA_LBA2
    out dx, al

    mov al, 0x20                    ; ATA_CMD_READ_PIO
    mov dx, ATA_CMD
    out dx, al

    mov ebx, CHUNK_SECTORS           ; ebx = сколько секторов ещё осталось
                                       ; прочитать в ЭТОМ чанке
.sector_loop:
    call ata_wait_bsy_clear
    call ata_wait_drq

    mov dx, ATA_DATA
    mov ecx, 256                     ; 256 слов = 512 байт на сектор
.word_loop:
    in ax, dx
    mov [edi], ax
    add edi, 2
    loop .word_loop

    dec ebx
    jnz .sector_loop

    add ebp, CHUNK_SECTORS
    dec esi
    jnz .chunk_loop
    ret

; вход/выход: ничего (портит только eax/edx через IN) -- ждёт, пока BSY
; сбросится. Таймаута сознательно нет: если диск завис -- зависание
; здесь настолько же диагностично, насколько был бы таймаут, а лишний
; путь "что делать при таймауте на этом этапе загрузки" всё равно
; свёлся бы к тому же hlt-циклу.
ata_wait_bsy_clear:
    mov dx, ATA_STATUS
.loop:
    in al, dx
    test al, ATA_SR_BSY
    jnz .loop
    ret

ata_wait_drq:
    mov dx, ATA_STATUS
.loop:
    in al, dx
    test al, ATA_SR_DRQ
    jnz .ready
    jmp .loop
.ready:
    ret

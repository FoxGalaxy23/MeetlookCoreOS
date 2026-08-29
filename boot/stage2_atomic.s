; stage2_atomic.s -- см. ATOMIC_PLAN.md, Фаза 6. Зеркало boot/stage2.s
; (тот же порядок действий: E820 -> A20 -- protected mode -- читаем
; ядро с диска через ATA PIO -- прыгаем в _start_native), но вместо
; ФИКСИРОВАННОГО LBA ядра читает boot-meta (include/bootmeta.h) и сам
; решает, из какого os-slot (A/B) грузить kernel.flat -- см. общий
; комментарий в apps/install-os.c (install_atomic_layout()) за полной
; разметкой диска.
;
; ГРУЗИТСЯ boot/stage1.s (та же parameterized версия, что и обычный
; stage1 -- см. общий комментарий там же, -D STAGE2_LBA_START=34), из
; раздела bios-boot (LBA 34..96, ATOMIC_BIOSBOOT_SECTORS в
; apps/install-os.c) -- НЕ из LBA 1..63, как в NOGRUB-схеме: там на
; Atomic-диске уже лежат первичный GPT-заголовок и таблица разделов.
;
; ВОСЕМЬ КОНСТАНТ НИЖЕ (BOOTMETA_LBA/OS_SLOT_*_LBA/...) ОБЯЗАНЫ ТОЧНО
; СОВПАДАТЬ с раскладкой, которую считает install_atomic_layout()
; (apps/install-os.c) -- ассемблер не может подключить C-заголовки, а
; сама раскладка (в отличие от, скажем, boot-meta на NOGRUB-диске)
; зависит от нескольких констант сразу, поэтому здесь -- ЯВНЫЙ,
; заранее посчитанный итог, а не формула. Если что-то в
; install_atomic_layout() изменится -- эти equ придётся пересчитать и
; поправить руками (тот же риск, что уже принят у NATIVE_ENTRY_OFF в
; boot/boot.s -- см. комментарий там же).
;
;   ATOMIC_GPT_RESERVED_SECTORS (34, gpt.h/install-os.c) -- protective
;     MBR (1) + первичный GPT-заголовок (1) + первичная таблица (32)
;   + ATOMIC_BIOSBOOT_SECTORS (63, install-os.c)          -- ЭТОТ файл
;   -----------------------------------------------------
;   = 97 = BOOTMETA_LBA (начало раздела boot-meta)
;   + ATOMIC_BOOTMETA_PART_SECTORS (8, install-os.c)
;   -----------------------------------------------------
;   = 105 = OS_SLOT_A_LBA (начало os-slot-a)
;   + ATOMIC_OS_SLOT_SECTORS (131072 = 64 MiB, install-os.c)
;   -----------------------------------------------------
;   = 131177 = OS_SLOT_B_LBA (начало os-slot-b)
;
; ДИАГНОСТИКА -- тот же приём, что и в stage2.s: сырой вывод в COM1,
; см. общий комментарий там же.

bits 16
org 0x8000

; ---- Общие константы ----
E820_BUFFER      equ 0x9000    ; см. stage2.s -- тот же формат/адрес,
                                 ; не переиспользуем этот файл вместе со
                                 ; stage2.s в одной сборке, коллизий нет
E820_MAX_ENTRIES equ 64
BOOTMETA_BUF     equ 0x9600    ; сектор boot-meta (512 байт) -- после
                                 ; E820_BUFFER (0x9000..0x94FF) с запасом
ATOMIC_INFO_OUT  equ 0x9800    ; исходящий блок для kernel/bootinfo_e820.c
                                 ; (ATOMIC_BOOTINFO_MAGIC, include/bootinfo.h) --
                                 ; после BOOTMETA_BUF (0x9600..0x97FF) с запасом
KERNEL_LOAD_ADDR equ 0x00100000 ; см. stage2.s -- тот же адрес
NATIVE_ENTRY_OFF equ 0x1000

; ---- Разметка диска -- см. общий комментарий в начале файла ----
BOOTMETA_LBA          equ 97
OS_SLOT_A_LBA         equ 105
OS_SLOT_B_LBA         equ 131177
KERNEL_SECTORS        equ 32768  ; см. stage2.s KERNEL_SECTORS (тот же бюджет, 16 MiB,
                                   ; поднято с 2, 4, затем 8 MiB -- см. её комментарий) --
                                   ; читаем область под kernel.flat С НАЧАЛА слота, не
                                   ; весь 64-МиБ слот целиком (тот же принцип, что и
                                   ; NOGRUB-схема -- слот просто щедрее размечен, чем
                                   ; реально используется, на будущий рост)

; ---- Формат boot-meta (include/bootmeta.h) -- см. bootmeta_serialize()/
; bootmeta_parse(), lib/bootmeta.c, за тем же самым в C ----
BOOTMETA_MAGIC        equ 0x4D4C4254 ; "MLBT"
BOOTMETA_SLOT_A       equ 0
BOOTMETA_SLOT_B       equ 1
BOOTMETA_DEFAULT_TRIES equ 3

; ---- Формат исходящего atomic-info блока -- см. ATOMIC_BOOTINFO_MAGIC,
; include/bootinfo.h, за той же раскладкой в C ----
ATOMIC_BOOTINFO_MAGIC equ 0x4D4C4142 ; "MLAB"

start:
    mov sp, 0x7C00              ; см. stage2.s -- тот же приём

    call init_serial16
    mov si, msg_start
    call serial_print16

    call get_e820
    call enable_a20

    cli
    lgdt [gdt32.pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp gdt32.code:pm_start

; ---- BIOS E820 -- байт в байт та же реализация, что stage2.s (см. её
; общий комментарий) -- намеренный дубль, оба файла грузятся В РАЗНОЕ
; время (взаимоисключающие сценарии установки: NOGRUB-диск либо
; Atomic-диск, никогда оба сразу), общий модуль между ними означал бы
; либо третий файл, либо усложнение сборки ради кода, который и так
; уместится в бюджет 63 секторов с запасом. ----
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
    mov edx, 0x534D4150
    mov ecx, 20
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done

    inc word [e820_count]
    add di, 20

    cmp word [e820_count], E820_MAX_ENTRIES
    jae .done

    test ebx, ebx
    jz .done
    jmp .loop

.done:
    pop ebx
    pop di
    pop es
    ret

enable_a20:
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE
    out 0x92, al
.done:
    ret

init_serial16:
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x03
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    ret

serial_putc16:
    push dx
    push ax
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

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

msg_start        db "stage2_atomic: start", 13, 10, 0
msg_bootmeta_bad db "stage2_atomic: boot-meta invalid -- halting", 13, 10, 0
msg_rollback     db "stage2_atomic: tries exhausted, rolling back to other slot", 13, 10, 0
msg_kernel_done  db "stage2_atomic: kernel loaded, jumping to native entry", 13, 10, 0

e820_count: dw 0

align 8
gdt32:
    dq 0
.code: equ $ - gdt32
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
.data: equ $ - gdt32
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
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
    mov esp, 0x7C00

    mov esi, msg_start
    call serial_print32   ; msg_start уже отпечатан в 16-битном коде,
                            ; но серийный порт не против повторов --
                            ; полезный маркер "мы точно в 32-битном коде"

    call load_bootmeta
    test eax, eax
    jz .bootmeta_bad

    call decide_slot        ; -> ebp = финальный active_slot (0/1),
                              ;    записывает boot-meta обратно на диск,
                              ;    если что-то изменилось (см. её комментарий)

    call load_kernel_from_slot   ; вход: ebp = active_slot

    mov esi, msg_kernel_done
    call serial_print32

    call build_atomic_info  ; заполняет ATOMIC_INFO_OUT из final_active_slot
                              ; (decide_slot), см. её комментарий

    mov eax, E820_BUFFER
    movzx ebx, word [e820_count]
    mov ecx, ATOMIC_INFO_OUT
    jmp KERNEL_LOAD_ADDR + NATIVE_ENTRY_OFF

.bootmeta_bad:
    mov esi, msg_bootmeta_bad
    call serial_print32
    cli
.hang:
    hlt
    jmp .hang

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

; ---------------------------------------------------------------------
; ---- Читает сектор boot-meta (LBA BOOTMETA_LBA) в BOOTMETA_BUF и
; ПРОВЕРЯЕТ его (magic + CRC32, см. bootmeta_parse(), lib/bootmeta.c) --
; выход: eax = 1 (валиден) либо 0 (magic/CRC не сошлись -- ОДНА ошибка
; на оба случая, тот же принцип, что и у bootmeta_parse() в C: "не
; совпало -- значит не оно", а не два разных диагноза; настоящего
; таймаута чтения здесь нет -- см. общий комментарий у ata_chunk_read
; ниже за тем же решением, что и в stage2.s). Портит eax/ebx/ecx/edx/esi/edi/ebp.
; ---------------------------------------------------------------------
load_bootmeta:
    mov edi, BOOTMETA_BUF
    mov ebp, BOOTMETA_LBA
    mov ebx, 1                     ; 1 сектор
    call ata_read_sectors

    ; magic -- offset 0, 4 байта LE
    mov eax, [BOOTMETA_BUF]
    cmp eax, BOOTMETA_MAGIC
    jne .fail

    ; сохранённый CRC32 (offset 8) должен совпасть с CRC32([0..8))
    mov esi, BOOTMETA_BUF
    mov ecx, 8
    call crc32_compute            ; eax = посчитанный CRC32 (сохраняет ebx/ecx/edx/esi)
    cmp eax, [BOOTMETA_BUF + 8]
    jne .fail

    mov eax, 1
    ret
.fail:
    xor eax, eax
    ret

; ---------------------------------------------------------------------
; ---- Решает, из какого слота грузиться (ATOMIC_PLAN.md, Фаза 6:
; tries/rollback/confirm) -- ПРЕДПОЛАГАЕТ, что load_bootmeta уже успешно
; заполнил BOOTMETA_BUF валидными данными. Возможно ПЕРЕПИСЫВАЕТ
; BOOTMETA_BUF на диске (см. три случая ниже). Итоговый active_slot
; сохраняется В ПАМЯТИ (final_active_slot), А НЕ в регистре -- ebp
; дальше используется load_kernel_from_slot как LBA-курсор (растущее
; значение через весь ata_read_sectors), поэтому полагаться на то, что
; ebp донесёт число 0/1 в неизменном виде до build_atomic_info, нельзя.
; Портит eax/ebx/ecx/edx/esi/edi/ebp.
; ---------------------------------------------------------------------
decide_slot:
    movzx eax, byte [BOOTMETA_BUF + 6]  ; boot_confirmed
    test eax, eax
    jnz .confirmed_fast_path             ; уже подтверждено -- грузим как есть, ничего не пишем

    movzx eax, byte [BOOTMETA_BUF + 5]   ; boot_tries_left
    test eax, eax
    jnz .decrement_and_go

    ; ---- Попытки исчерпаны -- автооткат на противоположный слот ----
    mov esi, msg_rollback
    call serial_print32

    movzx eax, byte [BOOTMETA_BUF + 4]   ; текущий active_slot
    xor eax, 1                            ; bootmeta_other_slot() -- ровно два
                                            ; значения (0/1), см. lib/bootmeta.c
    mov [BOOTMETA_BUF + 4], al
    mov byte [BOOTMETA_BUF + 5], 0        ; boot_tries_left = 0 -- та же комбинация,
                                            ; что bootmeta_mark_confirmed() (Фаза 6,
                                            ; lib/bootmeta.c): слот, на который
                                            ; откатились, был рабочим ДО обновления --
                                            ; считаем его подтверждённым сразу, иначе
                                            ; следующий обычный сбой этого же (старого,
                                            ; ранее рабочего) слота съел бы его попытки
                                            ; без всякой связи с апдейтом
    mov byte [BOOTMETA_BUF + 6], 1        ; boot_confirmed = 1
    call write_bootmeta
    jmp .store_and_return

.decrement_and_go:
    dec al
    mov [BOOTMETA_BUF + 5], al            ; boot_tries_left -= 1 -- "это была попытка"
    call write_bootmeta
    jmp .store_and_return

.confirmed_fast_path:
    ; ничего не пишем -- см. общий комментарий в начале функции

.store_and_return:
    movzx eax, byte [BOOTMETA_BUF + 4]
    mov [final_active_slot], al
    movzx ebp, byte [BOOTMETA_BUF + 4]    ; для load_kernel_from_slot сразу после нас
    ret

; ---------------------------------------------------------------------
; ---- Пересчитывает CRC32 над BOOTMETA_BUF[0..8) и пишет БУФЕР ЦЕЛИКОМ
; (все 512 байт -- офсеты 12..511 не менялись с момента load_bootmeta)
; обратно на LBA BOOTMETA_LBA. Портит eax/ecx/edx/esi/ebp.
; ---------------------------------------------------------------------
write_bootmeta:
    mov esi, BOOTMETA_BUF
    mov ecx, 8
    call crc32_compute
    mov [BOOTMETA_BUF + 8], eax

    mov esi, BOOTMETA_BUF
    mov ebp, BOOTMETA_LBA
    call ata_write_one_sector
    ret

; ---------------------------------------------------------------------
; ---- Читает kernel.flat из слота ebp (BOOTMETA_SLOT_A/B, ТОЛЬКО что
; выставлен decide_slot) в KERNEL_LOAD_ADDR. Портит eax/ebx/ecx/edx/esi/edi/ebp.
; ---------------------------------------------------------------------
load_kernel_from_slot:
    cmp ebp, BOOTMETA_SLOT_A
    je .slot_a
    mov ebp, OS_SLOT_B_LBA
    jmp .go
.slot_a:
    mov ebp, OS_SLOT_A_LBA
.go:
    mov edi, KERNEL_LOAD_ADDR
    mov ebx, KERNEL_SECTORS
    call ata_read_sectors
    ; Успех отдельно не проверяется -- см. общий комментарий у
    ; ata_chunk_read ниже: этот минимальный ATA-driver таймаутов не
    ; делает вовсе (зависание в ata_wait_bsy_clear/ata_wait_drq уже
    ; достаточно диагностично на последовательном порту -- та же
    ; философия, что и load_kernel в stage2.s).
    ret

; ---------------------------------------------------------------------
; ---- Заполняет ATOMIC_INFO_OUT (см. общий комментарий в начале файла
; и у ATOMIC_BOOTINFO_MAGIC, include/bootinfo.h) из final_active_slot
; (см. decide_slot -- НЕ из ebp, который к этому моменту уже перезаписан
; load_kernel_from_slot текущим LBA-курсором). Портит eax.
; ---------------------------------------------------------------------
build_atomic_info:
    mov dword [ATOMIC_INFO_OUT], ATOMIC_BOOTINFO_MAGIC
    mov byte  [ATOMIC_INFO_OUT + 4], 0     ; drive_index -- см. общий комментарий у
                                             ; atomic_boot_drive, include/bootinfo.h:
                                             ; ВСЕГДА 0 (primary master) -- тот же
                                             ; единственный канал/диск, который читает
                                             ; и пишет весь ATA-код в этом файле
    mov al, [final_active_slot]
    mov byte [ATOMIC_INFO_OUT + 5], al
    mov word [ATOMIC_INFO_OUT + 6], 0       ; reserved
    mov dword [ATOMIC_INFO_OUT + 8], BOOTMETA_LBA
    ret

final_active_slot: db 0

; ---------------------------------------------------------------------
; ---- CRC32 (CRC-32/ISO-HDLC, полином 0xEDB88320) -- бит-последовательная
; реализация, байт в байт эквивалентная табличной lib/crc32.c::crc32()
; при initial=0 (см. её общий комментарий за тем, почему это стандартный
; алгоритм с init/финальным XOR 0xFFFFFFFF, несмотря на имя параметра
; "initial"). Таблица (256*4 байт) здесь сознательно не заведена -- вход
; всегда 8 байт (заголовок boot-meta), 64 бита-итерации
; бит-последовательного варианта дешевле и, что важнее в асме без
; отладчика, на порядок проще писать без ошибки, чем 1 КиБ таблицы руками.
; вход: esi = буфер, ecx = длина в байтах
; выход: eax = CRC32
; портит: только eax (ebx/ecx/edx/esi сохраняет через push/pop)
; ---------------------------------------------------------------------
crc32_compute:
    push ebx
    push ecx
    push edx
    push esi
    mov eax, 0xFFFFFFFF
.byte_loop:
    test ecx, ecx
    jz .done
    movzx edx, byte [esi]
    xor al, dl
    mov ebx, 8
.bit_loop:
    shr eax, 1
    jnc .no_xor
    xor eax, 0xEDB88320
.no_xor:
    dec ebx
    jnz .bit_loop
    inc esi
    dec ecx
    jmp .byte_loop
.done:
    xor eax, 0xFFFFFFFF
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

; ---------------------------------------------------------------------
; ---- Минимальный ATA PIO (LBA28), ТОЛЬКО первичный канал (0x1F0),
; ТОЛЬКО master -- байт в байт тот же протокол, что stage2.s (см. её
; общий комментарий за тем, почему это неизбежный дубль drivers/ide.c,
; и за тем же самым решением "таймаутов нет, зависание диагностично
; само по себе через serial"). ЧТЕНИЕ здесь дополнено ЗАПИСЬЮ одного
; сектора (нужна write_bootmeta выше -- stage2.s читает диск, но
; никогда не пишет).
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
ATA_CMD_READ  equ 0x20
ATA_CMD_WRITE equ 0x30
ATA_CMD_FLUSH equ 0xE7

CHUNK_SECTORS equ 128   ; тот же бюджет, что stage2.s -- влезает с запасом
                          ; в 8-битный SECCOUNT0 (макс. 255)

; ---- ata_read_sectors: читает ebx секторов (>=1) начиная с LBA ebp в
; edi, чанками по CHUNK_SECTORS -- работает для ЛЮБОГО ebx (включая < 
; CHUNK_SECTORS, например ebx=1 у load_bootmeta -- никакого отдельного
; "хвостового" случая не нужно, в отличие от ранней, ошибочной версии
; этого файла).
; вход: edi, ebp, ebx (>=1)
; портит: eax/ebx/ecx/edx/edi/ebp (ebx уходит в 0; edi/ebp продвигаются
; на прочитанный объём)
ata_read_sectors:
.loop:
    test ebx, ebx
    jz .done
    mov ecx, CHUNK_SECTORS
    cmp ebx, ecx
    jae .have_chunk
    mov ecx, ebx                   ; последний, неполный чанк (<CHUNK_SECTORS)
.have_chunk:
    call ata_chunk_read            ; вход: edi, ebp, ecx (<=255) -- ecx/ebp
                                     ; ПОСЛЕ возврата равны тому, что было ДО
                                     ; вызова (см. её комментарий) -- edi
                                     ; продвинут на ecx*512 байт
    add ebp, ecx
    sub ebx, ecx
    jmp .loop
.done:
    ret

; ---- ata_chunk_read: читает ecx (1..255) секторов начиная с LBA ebp в
; edi через ATA PIO READ. ПРОДВИГАЕТ edi (естественный побочный эффект
; посекторной, послословной передачи через порт данных) -- ecx/ebp
; ПОСЛЕ возврата равны тому, что было ДО вызова: сохраняются через
; push/pop, а внутри используется ОТДЕЛЬНАЯ копия счётчика секторов в
; ebx (не в ecx), потому что ecx нужен инструкции LOOP для внутреннего
; цикла "256 слов на сектор" ОДНОВРЕМЕННО с внешним "сколько секторов в
; этом чанке осталось" -- один регистр не может считать оба сразу.
; вход: edi, ebp, ecx (1..255)
; портит: eax, edx; edi -- продвигает (результат, не порча); ebp/ecx --
; не меняются
ata_chunk_read:
    push ebx
    push ecx
    mov ebx, ecx
    call ata_wait_bsy_clear

    mov eax, ebp
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_DRVSEL
    out dx, al

    mov al, bl
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

    mov al, ATA_CMD_READ
    mov dx, ATA_CMD
    out dx, al

.sector_loop:
    call ata_wait_bsy_clear
    call ata_wait_drq
    mov dx, ATA_DATA
    mov ecx, 256
.word_loop:
    in ax, dx
    mov [edi], ax
    add edi, 2
    loop .word_loop
    dec ebx
    jnz .sector_loop

    pop ecx
    pop ebx
    ret

; ---- ata_write_one_sector: пишет РОВНО 1 сектор из esi на LBA ebp
; через ATA PIO WRITE, затем CACHE FLUSH (см. drivers/ide.c::
; ide_write_sectors() -- тот же протокол, гарантирует, что запись реально
; ушла на носитель прежде, чем мы, возможно, прочитаем её же обратно на
; следующей загрузке). Всегда ровно 1 сектор -- единственный вызывающий
; код (write_bootmeta) ничего больше не пишет, обобщать до чанков ради
; него незачем (в отличие от чтения, которому реально нужны произвольные
; объёмы -- kernel.flat).
; вход: esi, ebp
; портит: eax/ecx/edx (esi/ebp -- сохраняет через push/pop, хоть и не
; нужны вызывающему коду после возврата -- меньше сюрпризов для
; будущего кода, который вызовет эту функцию ещё откуда-то)
ata_write_one_sector:
    push esi
    push ebp
    call ata_wait_bsy_clear

    mov eax, ebp
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_DRVSEL
    out dx, al

    mov al, 1
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

    mov al, ATA_CMD_WRITE
    mov dx, ATA_CMD
    out dx, al

    call ata_wait_bsy_clear
    call ata_wait_drq
    mov dx, ATA_DATA
    mov ecx, 256
.word_loop:
    mov ax, [esi]
    out dx, ax
    add esi, 2
    loop .word_loop

    mov dx, ATA_CMD
    mov al, ATA_CMD_FLUSH
    out dx, al
    call ata_wait_bsy_clear

    pop ebp
    pop esi
    ret

ata_wait_bsy_clear:
    push eax
    push edx
    mov dx, ATA_STATUS
.loop:
    in al, dx
    test al, ATA_SR_BSY
    jnz .loop
    pop edx
    pop eax
    ret

ata_wait_drq:
    push eax
    push edx
    mov dx, ATA_STATUS
.loop:
    in al, dx
    test al, ATA_SR_DRQ
    jz .loop
    pop edx
    pop eax
    ret

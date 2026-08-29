; stage1.s -- MBR-загрузчик (сектор 0, 512 байт). См. NOGRUB_PLAN.md,
; Фаза B. Единственная задача: прочитать stage2 (LBA 1..63 -- см. общую
; разметку диска в NOGRUB_PLAN.md) и передать ему управление.
;
; ДВА ПУТИ ЧТЕНИЯ ДИСКА -- см. NOGRUB_PLAN.md, раздел "ISO без GRUB
; (El Torito)" за полной историей отладки. Коротко: расширенный INT 13h
; (AH=0x42, LBA-адресация) работает на реальном диске (наш основной
; install-путь через install-os, полностью проверено) и умеет читать
; под El Torito -no-emul-boot (расширения ЕСТЬ), но у SeaBIOS's El
; Torito -hard-disk-boot эмуляции расширенного INT 13h для эмулированного
; диска попросту НЕТ (подтверждено эмпирически: AH=0x41 "Check
; Extensions Present" возвращает CF=1). Поэтому:
;   1) при старте проверяем INT 13h AH=0x41;
;   2) если расширения ЕСТЬ -- читаем через DAP (AH=0x42), как обычно;
;   3) если расширений НЕТ -- читаем через legacy CHS (AH=0x02),
;      переводя LBA в CHS по геометрии от самого BIOS (AH=0x08).
; Полный ISO без GRUB (через El Torito) при этом ВСЁ РАВНО не заработал
; -- см. NOGRUB_PLAN.md за деталями (у -no-emul-boot есть расширения, но
; сам расширенный CD-ROM read падает с BIOS-ошибкой 0x09 "data boundary
; error" по причинам, которые требуют разбора исходников SeaBIOS, а не
; догадок; у -hard-disk-boot ещё и BIOS AH=0x08 возвращает вырожденную
; геометрию, CHS-fallback тоже не спасает). Эта логика тем не менее
; остаётся -- она делает stage1 объективно надёжнее на реальном
; железе/BIOS'ах, которые могут не поддерживать EDD, и корректно
; сообщает об ошибке вместо зависания, если что-то пойдёт не так.

bits 16
org 0x7C00

STAGE2_LOAD_SEG   equ 0x0000
STAGE2_LOAD_OFF   equ 0x8000     ; stage2 грузится по линейному адресу 0x8000
; STAGE2_LBA_START/STAGE2_SECTORS -- переопределяемы через `nasm -D` (см.
; Makefile, цель build/stage1_gpt.bin, ATOMIC_PLAN.md Фаза 6): ЭТОТ ЖЕ
; файл, без единой другой правки, используется и для обычного
; (NOGRUB, фиксированный LBA) диска, и для protective MBR на
; GPT/Atomic-диске -- единственная разница между ними -- ГДЕ на диске
; лежит stage2 (см. apps/install-os.c: ATOMIC_BIOSBOOT_SECTORS/LBA 34 --
; после GPT-заголовка и таблицы разделов, а не сразу после LBA 0, как
; здесь по умолчанию). Значение по умолчанию ниже -- LBA 1..63, ровно
; та же разметка, что и раньше (NOGRUB_PLAN.md) -- этот %ifndef ничего
; не меняет в поведении обычной (не-Atomic) сборки.
%ifndef STAGE2_LBA_START
STAGE2_LBA_START  equ 1           ; см. разметку диска, NOGRUB_PLAN.md
%endif
%ifndef STAGE2_SECTORS
STAGE2_SECTORS    equ 63          ; LBA 1..63 включительно
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    ; ---- Есть ли расширенный (LBA/EDD) INT 13h для этого диска? ----
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .use_chs                  ; CF=1 -- расширений нет

    ; ---- Есть расширения -- читаем через DAP ----
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

.use_chs:
    call read_stage2_chs
    jc disk_error
    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

; ---------------------------------------------------------------------
; ---- Чтение stage2 через legacy CHS (для сред без EDD) -- читает
; STAGE2_SECTORS секторов начиная с STAGE2_LBA_START в
; STAGE2_LOAD_SEG:STAGE2_LOAD_OFF, по одному сектору за раз. Возвращает
; CF=1 при ошибке (в т.ч. если BIOS не дал вменяемую геометрию --
; 0 секторов/дорожку -- вместо зависания на делении на ноль).
; ---------------------------------------------------------------------
read_stage2_chs:
    push ax
    push bx
    push cx
    push dx
    push es

    mov ah, 0x08
    mov dl, [boot_drive]
    xor di, di
    push es
    push ds
    pop es
    mov es, di
    int 0x13
    pop es
    jc .fail
    and cl, 0x3F
    jz .fail                      ; 0 секторов/дорожку -- вменяемой геометрии нет,
                                   ; без этой проверки следующий div уйдёт в
                                   ; деление на ноль и намертво зависнет
    mov [sectors_per_track], cl
    movzx ax, dh
    inc ax
    mov [heads_count], ax

    mov word [cur_lba], STAGE2_LBA_START
    mov word [remaining], STAGE2_SECTORS
    mov ax, STAGE2_LOAD_SEG
    mov es, ax
    mov bx, STAGE2_LOAD_OFF

.loop:
    cmp word [remaining], 0
    je .done

    mov ax, [cur_lba]
    xor dx, dx
    movzx cx, byte [sectors_per_track]
    div cx                        ; ax = lba/spt, dx = lba%spt
    inc dx
    mov cl, dl                    ; номер сектора (1-indexed)

    xor dx, dx
    div word [heads_count]        ; ax = cylinder, dx = head
    mov ch, al
    mov dh, dl

    mov dl, [boot_drive]
    mov ah, 0x02
    mov al, 1
    int 0x13
    jc .fail

    add bx, 512
    inc word [cur_lba]
    dec word [remaining]
    jmp .loop

.done:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

sectors_per_track: db 0
heads_count:        dw 0
cur_lba:             dw 0
remaining:           dw 0

disk_error:
    mov si, msg_disk_error
.print:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    int 0x10
    jmp .print
.hang:
    cli
.spin:
    hlt
    jmp .spin

boot_drive:     db 0
msg_disk_error: db "stage1: disk read error", 0

align 4
dap:
    db 0x10
    db 0
    dw STAGE2_SECTORS
    dw STAGE2_LOAD_OFF
    dw STAGE2_LOAD_SEG
    dq STAGE2_LBA_START

times 446 - ($ - $$) db 0

; Таблица разделов на этом сыром тестовом образе (Фаза B) пустая --
; ни stage1, ни stage2 её не читают, оба работают с фиксированными LBA
; напрямую. install-os (Фаза 4 INSTALLER_PLAN.md) впишет сюда настоящую
; FAT32-партицию через mbr_build() (lib/mbr.c) поверх ЭТОГО ЖЕ boot_code.
times 16 * 4 db 0

dw 0xAA55

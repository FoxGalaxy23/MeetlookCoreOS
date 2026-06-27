; boot.asm — Stage 1 Bootloader (assembled as flat binary at 0x7C00)
[BITS 16]
[ORG 0x7C00]

_start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti

    ; Сохраняем номер диска
    mov  [boot_drive], dl

    ; Сообщение о загрузке
    mov  si, msg_boot
    call print_str

    ; Читаем 16 секторов (= 8KB) начиная с сектора 2 в 0x0000:0x1000
    mov  ax, 0x0000
    mov  es, ax
    mov  bx, 0x1000

    mov  ah, 0x02            ; BIOS Read Sectors
    mov  al, 16              ; кол-во секторов
    mov  ch, 0               ; цилиндр 0
    mov  cl, 2               ; сектор 2 (1-indexed)
    mov  dh, 0               ; головка 0
    mov  dl, [boot_drive]
    int  0x13
    jc   disk_error

    ; Прыгаем в ядро
    jmp  0x0000:0x1000

disk_error:
    mov  si, msg_err
    call print_str
.hang:
    hlt
    jmp  .hang

; --- print_str: SI -> zero-terminated string ---
print_str:
    pusha
.loop:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    mov  bx, 0x0007
    int  0x10
    jmp  .loop
.done:
    popa
    ret

; --- Данные ---
msg_boot   db "Loading MeetlookCoreOS...", 0x0D, 0x0A, 0
msg_err    db "Disk read error!", 0x0D, 0x0A, 0
boot_drive db 0

; Boot signature
TIMES 510 - ($ - $$) db 0
DW 0xAA55
; kernel_entry.asm — первые байты ядра, линкуется раньше main.o
; Вызывается прыжком из загрузчика (0x0000:0x1000)
[BITS 16]

global _kstart
extern kmain

section .text.entry          ; секция с особым именем — будет первой

_kstart:
    ; Выровняем сегментные регистры
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7000          ; новый стек подальше от ядра

    call kmain               ; вызываем C-код

.hang:
    cli
    hlt
    jmp  .hang

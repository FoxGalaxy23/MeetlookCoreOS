; boot.asm — точка входа и инициализация
[BITS 16]

global _start
extern kmain        ; C-функция из main.c

section .text

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call kmain      ; вызываем C-код

    ; Бесконечный цикл после завершения
    cli
.hang:
    hlt
    jmp .hang
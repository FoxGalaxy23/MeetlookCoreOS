; ap_trampoline_blob.s -- встраивает build/ap_trampoline.bin (см.
; boot/ap_trampoline.s и правило сборки в Makefile) как обычный массив
; байт внутрь ELF-образа ядра, чтобы kernel/smp.c мог скопировать его
; в физическую память по фиксированному адресу (AP_TRAMPOLINE_BASE,
; include/smp.h) во время выполнения -- см. smp_init().
;
; Плоский бинарник (собранный `nasm -f bin`, 16-битный код) и обычные
; объектники ядра (`nasm -f elf64`) -- РАЗНЫЕ форматы, слинковать их
; напрямую с остальным ядром нельзя; incbin здесь просто копирует байты
; как есть в секцию .rodata текущего (уже elf64) файла, ничего не
; интерпретируя.

section .rodata
align 4096

global ap_trampoline_blob
global ap_trampoline_blob_end

ap_trampoline_blob:
    incbin "build/ap_trampoline.bin"
ap_trampoline_blob_end:

section .note.GNU-stack noalloc noexec nowrite progbits

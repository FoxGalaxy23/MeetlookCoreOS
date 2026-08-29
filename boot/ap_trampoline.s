; ap_trampoline.s -- код, который реально выполняет каждый Application
; Processor (AP) сразу после INIT-SIPI-SIPI (см. kernel/apic.c,
; kernel/smp.c), в самом начале своей жизни.
;
; ПОЧЕМУ ЭТО ОТДЕЛЬНЫЙ ФАЙЛ, СОБИРАЕМЫЙ ИНАЧЕ, ЧЕМ ВСЁ ОСТАЛЬНОЕ ЯДРО:
; при получении Startup IPI ядро AP начинает выполнение РОВНО как
; настоящий 8086 сразу после сброса -- 16-битный real mode, CS:IP =
; (vector*0x100):0x0000, где vector -- байт, переданный в SIPI (см.
; lapic_send_sipi() в apic.c). Это тот же самый режим, с которого когда-то
; начинался весь boot.s (см. общий комментарий там), только теперь БЕЗ
; помощи GRUB/Multiboot -- сюда нас приводит не загрузчик, а другое ядро
; той же машины. Раз режим 16-битный, код обязан быть собран как чистый
; плоский бинарник (`nasm -f bin`, см. правило в Makefile), без ELF
; заголовков и релокаций -- обычные .o из остального ядра здесь
; неприменимы, компоновщик ld ожидает совсем других адресных соглашений.
;
; Смысл: пройти те же самые три стадии, что когда-то boot.s (16-бит ->
; 32-бит protected mode -> 64-бит long mode), но НЕ строя заново таблицы
; страниц -- AP использует ТОТ ЖЕ САМЫЙ PML4, что BSP уже построил и
; продолжает использовать (см. paging_kernel_cr3() в kernel/paging.c) --
; ведь "ядерное" адресное пространство (identity map нижних 4 GiB) одно
; на всю машину, а не по одному на процессор. Единственное, что у
; каждого AP обязано быть своим с самого первого же мгновения в 64-битном
; режиме -- это стек (см. ap_mailbox_stack_top ниже, kernel/smp.c кладёт
; туда адрес заранее выделенного персонального стека ДО того, как послать
; этому конкретному AP SIPI).
;
; "Почтовый ящик" (mailbox) -- фиксированная область в самом конце этой
; же страницы (смещения 0xFE0-0xFFF от начала трамплина, см.
; AP_TRAMPOLINE_MAILBOX_OFFSET в include/smp.h): туда BSP заранее (см.
; smp_start_ap() в kernel/smp.c) записывает физический адрес PML4, вершину
; стека этого конкретного AP, индекс этого AP (передаётся первым
; аргументом в ap_entry64) и адрес самой функции ap_entry64 (см.
; kernel/smp.c) -- единственное место, где 16-битный/32-битный код этого
; файла вообще "видит" что-либо из настоящего 64-битного ядра.
;
; ORG 0x8000: физический адрес, на который BSP копирует этот блок перед
; тем, как стартовать AP (см. smp_init() в kernel/smp.c) -- совпадает с
; вектором SIPI 0x08 (0x08 * 0x1000 = 0x8000, см. lapic_send_sipi()).
; Внутри первого зарезервированного PMM-мегабайта (см. include/pmm.h),
; identity-мапленного наравне со всем остальным -- то есть безопасен и
; заведомо не пересекается ни с кучей ядра, ни с чем-либо ещё.

BITS 16
ORG 0x8000

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [ap_gdt32_ptr]

    mov eax, cr0
    or eax, 1              ; CR0.PE -- protected mode
    mov cr0, eax

    jmp 0x08:ap_prot32          ; дальний переход -- сбрасывает конвейер выборки
                                ; и наконец по-настоящему делает CS = 32-битным
                                ; кодовым селектором (тот же трюк, что и в boot.s,
                                ; см. jmp gdt64.code:long_mode_start там же)

BITS 32
ap_prot32:
    mov ax, 0x10               ; наш собственный плоский data-селектор (см. ap_gdt32 ниже)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE -- необходим для long mode
    mov cr4, eax

    ; Единый на всю машину PML4 (см. общий комментарий выше) -- BSP кладёт
    ; его физический адрес в почтовый ящик один раз, до первого же SIPI
    ; (см. smp_init()), одинаково для всех AP.
    mov eax, [ap_mailbox_pml4_phys]
    mov cr3, eax

    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    or eax, 1 << 8                ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                ; CR0.PG
    mov cr0, eax

    ; КРИТИЧНО: включаем SSE ровно как boot.s делает для BSP (см.
    ; enable_sse в boot/boot.s) -- ДО перехода в long mode и уж тем
    ; более до первой же строчки Си-кода (kernel/smp.c: ap_entry64()).
    ; Без этого CR0.EM всё ещё установлен (эмуляция FPU/SSE) и
    ; CR4.OSFXSR/OSXMMEXCPT сброшены -- первая же SSE-инструкция,
    ; которую compiler сгенерирует для memset()/memcpy() даже на
    ; крошечных структурах (см. percpu_init_early() в kernel/percpu.c --
    ; типичная оптимизация -O2 на x86_64), немедленно даёт #UD. А
    ; поскольку IDT на этом ядре ещё не загружена (idt_load_current()
    ; выполняется уже В Си-коде, ПОСЛЕ этого места) и GDT ещё временная,
    ; такое #UD не находит куда провалиться и превращается в double,
    ; а следом в triple fault -- именно так выглядит "компьютер
    ; перезагружается в цикле" при попытке поднять больше одного ядра.
    mov eax, cr0
    and ax, 0xFFFB                ; сбрасываем CR0.EM (бит 2)
    or ax, 0x2                     ; ставим CR0.MP (бит 1)
    mov cr0, eax

    mov eax, cr4
    or eax, 3 << 9                  ; CR4.OSFXSR (бит 9) и CR4.OSXMMEXCPT (бит 10)
    mov cr4, eax

    lgdt [ap_gdt64_ptr]
    jmp 0x08:ap_long64

BITS 64
ap_long64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Персональный стек этого AP -- BSP кладёт его СЮДА (в тот же самый
    ; почтовый ящик) непосредственно перед отправкой SIPI именно этому
    ; ядру (см. smp_start_ap() в kernel/smp.c) -- в отличие от PML4 выше,
    ; это значение меняется перед КАЖДЫМ запускаемым AP.
    mov rsp, [ap_mailbox_stack_top]

    ; 1-й аргумент по System V AMD64 ABI (rdi) для ap_entry64(u32 cpu_index) --
    ; читаем как 32-битный edi, верхняя половина rdi обнулится автоматически.
    mov edi, [ap_mailbox_cpu_index]

    mov rax, [ap_mailbox_entry_point]
    jmp rax                      ; в kernel/smp.c: ap_entry64() -- дальше уже
                                  ; обычный 64-битный Си-код, ничем не отличающийся
                                  ; от кода, который выполняет BSP

; ---- Временные GDT для 32-битной и 64-битной стадий ----
; Ровно два дескриптора в каждой (null + code + data, как и временная
; gdt64 в boot/boot.s) -- этого достаточно, чтобы сразу же после прыжка
; в long_mode/protected_mode переключиться на НАСТОЯЩУЮ GDT ядра
; (gdt_init_ap(), см. kernel/gdt.c) первым же делом внутри ap_entry64().
align 8
ap_gdt32:
    dq 0
    dq 0x00CF9A000000FFFF   ; 32-битный код, base=0, limit=4GiB, present, DPL=0
    dq 0x00CF92000000FFFF   ; 32-битные данные, base=0, limit=4GiB, present, DPL=0
ap_gdt32_ptr:
    dw $ - ap_gdt32 - 1
    dd ap_gdt32

align 8
ap_gdt64:
    dq 0
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)  ; код, present, long mode (L=1) -- как gdt64 в boot.s
    dq (1<<44) | (1<<47) | (1<<41)            ; данные, present, writable
ap_gdt64_ptr:
    dw $ - ap_gdt64 - 1
    dq ap_gdt64

; ---- Почтовый ящик: см. AP_TRAMPOLINE_MAILBOX_OFFSET в include/smp.h ----
; Явно прижимаем к смещению 0xFE0 от начала страницы -- kernel/smp.c
; вычисляет физические адреса этих четырёх полей как
; (TRAMPOLINE_BASE + 0xFE0 + смещение), а не по меткам (плоский бинарник
; не несёт таблицы символов наружу) -- порядок и размер полей здесь
; ОБЯЗАН совпадать один-в-один с тем, что предполагает smp.c.
times 0x0FE0 - ($ - $$) db 0
ap_mailbox_pml4_phys:    dq 0
ap_mailbox_stack_top:    dq 0
ap_mailbox_entry_point:  dq 0
ap_mailbox_cpu_index:    dq 0

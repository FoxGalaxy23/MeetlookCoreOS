; smp_isr.s -- заглушки прерываний, специфичных для SMP/LAPIC (см.
; include/apic.h за назначением каждого вектора). Сознательно ОТДЕЛЬНЫЙ
; файл от kernel/isr.s: те 48 заглушек (isr0..isr31, irq0..irq15)
; диспетчеризуются через общий isr_common_stub/irq_common_stub с полным
; registers_t и таблицей обработчиков isr.c (векторы 0-47, см.
; include/isr.h) -- под LAPIC-прерывания (векторы 0xF0-0xFF) заводить
; ещё одну запись в ТУ ЖЕ таблицу не нужно: у каждого ровно один,
; заранее известный обработчик, и полный снимок регистров им не нужен
; (ни один из них не читает и тем более не подменяет состояние
; прерванного кода, в отличие, например, от таймерного IRQ0, который
; переключает задачи).
;
; Тем не менее регистры общего назначения ПО-ПРЕЖНЕМУ обязаны быть
; сохранены и восстановлены -- Си-обработчик (см. kernel/smp.c) вправе
; их использовать, а прерванный код рассчитывает получить их в точности
; такими же, какими они были до прерывания.

global lapic_timer_isr
global ipi_reschedule_isr
global ipi_call_isr
global lapic_spurious_isr

extern lapic_timer_c_handler
extern ipi_reschedule_c_handler
extern ipi_call_c_handler

%macro SAVE_ALL 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_ALL 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; Вектор LAPIC_TIMER_VECTOR (0xF0, см. include/apic.h) -- локальный
; таймер этого ядра (запрограммирован в lapic_timer_start_periodic(),
; kernel/apic.c). Роль ровно та же, что у timer_irq_handler() для BSP
; (kernel/timer.c) -- "пульс" планировщика, см. scheduler_tick() в
; kernel/task.c -- но приходит из LAPIC, а не из PIC, поэтому и EOI
; шлётся в LAPIC (lapic_eoi(), см. lapic_timer_c_handler() в kernel/smp.c),
; а не на 8259.
lapic_timer_isr:
    SAVE_ALL
    call lapic_timer_c_handler
    RESTORE_ALL
    iretq

; Вектор LAPIC_IPI_RESCHEDULE (0xFB) -- см. smp_kick_idle_cpus() в
; include/smp.h: будит ядро, простаивающее в HLT (см. ap_idle_entry() в
; kernel/smp.c), ничего специально не проверяя -- само прерывание уже
; прервало HLT, следующий оборот цикла простоя сам заглянет в общую
; очередь планировщика.
ipi_reschedule_isr:
    SAVE_ALL
    call ipi_reschedule_c_handler
    RESTORE_ALL
    iretq

; Вектор LAPIC_IPI_CALL (0xFC) -- зарезервированная инфраструктура для
; будущих межъядерных вызовов (например, TLB shootdown при более полной
; поддержке многопоточных процессов, см. общий комментарий в
; include/smp.h про то, почему это пока не требуется для ТЕКУЩЕЙ модели
; процессов этой ОС) -- на сегодня просто подтверждает получение.
ipi_call_isr:
    SAVE_ALL
    call ipi_call_c_handler
    RESTORE_ALL
    iretq

; Spurious Interrupt (см. LAPIC_SPURIOUS_VECTOR, include/apic.h) -- по
; спецификации Intel SDM Vol.3 §10.9 для НЕЁ EOI посылать НЕ нужно (в
; отличие от всех остальных векторов LAPIC выше) и, поскольку сам
; обработчик ничего не делает, ему незачем трогать вообще какие-либо
; регистры -- достаточно немедленного iretq.
lapic_spurious_isr:
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits

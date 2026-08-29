; isr.s -- заглушки исключений процессора (векторы 0-31) и аппаратных
; прерываний (IRQ0-15, ремапнутых на векторы 32-47, см. include/pic.h).
;
; Зачем вообще нужны заглушки, а не сразу C-функция как обработчик:
; во-первых, часть исключений CPU сама кладёт на стек 64-битный код
; ошибки, а часть -- нет, и в C нет способа "прочитать" это иначе, чем
; заранее выровняв стек в асме (для векторов без кода ошибки кладём туда
; фиктивный 0, чтобы форма стека была всегда одинаковой). Во-вторых, на
; входе в обработчик CPU сохраняет только SS/RSP/RFLAGS/CS/RIP -- регистры
; общего назначения (rax, rbx, ...) остаются как были в прерванном коде,
; и раз прерывание может случиться в буквальном смысле в любой точке
; исполнения, их обязательно нужно сохранить и потом восстановить.
;
; Каждая заглушка кладёт (код_ошибки, номер_вектора) и прыгает в общий
; обработчик, который уже сохраняет все регистры общего назначения,
; вызывает C-диспетчер (isr_handler/irq_handler, см. isr.c) с указателем
; на этот снимок регистров, и после возврата восстанавливает всё обратно.
;
; ВАЖНО для планировщика: контекст задачи (task.c) переключается НЕ
; здесь, а глубже -- внутри вызова irq_handler -> scheduler_tick ->
; context_switch (см. kernel/context.s). Тот механизм не использует
; iretq вообще: он просто меняет rsp и делает обычный ret, используя
; то, что вызванная отсюда C-функция всё равно рано или поздно вернётся
; сюда же (в исходную задачу) обычным образом, когда таймер снова выберет
; именно её -- и тогда мы дойдём до pop-ов и iretq ниже уже для неё.

extern isr_handler
extern irq_handler
extern syscall_isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; фиктивный код ошибки -- этот вектор его не даёт
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1        ; код ошибки уже положен самим CPU перед этим
    jmp isr_common_stub
%endmacro

%macro IRQ_STUB 2
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro

; ---- Исключения процессора 0-31 ----
; Коды ошибок реально кладёт CPU для векторов: 8, 10, 11, 12, 13, 14, 17,
; 21, 29, 30 (см. Intel SDM Vol.3, гл. 6.15 / AMD64 APM Vol.2, гл. 8.2).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; ---- Аппаратные прерывания IRQ0-15 -> векторы 32-47 (после pic_remap) ----
IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

%macro PUSH_ALL_REGS 0
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

%macro POP_ALL_REGS 0
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

; Порядок PUSH_ALL_REGS (rax первым .. r15 последним) специально подобран
; так, чтобы итоговая раскладка в памяти совпадала с полями registers_t
; (include/isr.h) сверху вниз: r15 лежит по самому маленькому адресу
; (там же, где после всех push'ей окажется rsp), rax -- сразу перед
; int_no/err_code/rip/... которые были положены раньше (см. выше).

; System V AMD64 ABI требует rsp % 16 == 0 непосредственно перед каждым
; call (иначе call/эпилог функции с SSE-инструкциями типа movaps на
; локальных массивах может упасть на невыровненном обращении). Прерывание
; может случиться в буквальном смысле на любой инструкции прерванной
; задачи, поэтому её исходный rsp мог быть выровнен как угодно -- нельзя
; просто взять текущий rsp и звать call. rbx используется как временное
; хранилище точного адреса registers_t: именно он, а не текущий
; (возможно, искусственно подровненный) rsp, идёт первым аргументом в
; C-обработчик и именно к нему возвращаемся перед POP_ALL_REGS.
;
; ВАЖНО: это ОБЯЗАНО быть callee-saved регистр (rbx/rbp/r12-r15), а НЕ
; r11 (как было раньше) -- r11 в System V AMD64 ABI caller-saved, то есть
; ЛЮБАЯ вызванная C-функция (здесь -- isr_handler, а транзитивно и всё,
; что она сама вызывает: kprintf, kmalloc, paging_handle_page_fault и
; т.д.) вправе перезаписать r11 как обычный scratch-регистр, ничего не
; восстанавливая. На практике это происходит ровно тогда, когда
; обработчик прерывания делает что-то чуть сложнее тривиального: тогда
; "mov rsp, r11" ниже загружал в rsp не адрес registers_t, а случайное
; число, оставшееся в r11 от вычислений где-то в глубине вызова -- и
; последующий iretq валился с #GP на совершенно постороннем "стеке"
; (внешне выглядело как крах/перезагрузка всей системы при запуске ЛЮБОЙ
; sandboxed-программы, а не только каких-то конкретных). rbx же обязан
; быть сохранён и восстановлен любым корректным callee по этому ABI --
; а его СОБСТВЕННОЕ (принадлежащее прерванной задаче) значение к этому
; моменту уже в безопасности на стеке, ведь PUSH_ALL_REGS выше его уже
; туда положил; POP_ALL_REGS позже вернёт его как ни в чём не бывало.
; ВАЖНО (см. предыдущий комментарий про r11 -> rbx): даже callee-saved
; регистр -- это всё ещё ЕДИНЫЙ физический регистр на весь CPU, а не
; что-то личное для одной задачи. Если C-обработчик (isr_handler /
; irq_handler / syscall_isr_handler) где-то в глубине своего вызова
; попадёт в scheduler_tick() -> schedule() -> context_switch() и
; переключится на ДРУГУЮ задачу (или наша задача сама заблокируется и
; будет возобновлена позже -- ровно так работает sys_irq_wait(),
; kernel/syscall.c), то к моменту, когда мы наконец вернёмся из call и
; сделаем "mov rsp, rbx", это все ещё технически ТОТ ЖЕ регистр rbx,
; чьё сохранение целиком полагается на то, что КАЖДАЯ функция в этой
; длинной, потенциально прерывающейся на переключение контекста цепочке
; идеально соблюдает ABI. Это трудно доказать по построению и легко
; сломать неочевидным образом -- поэтому вместо регистра храним точный
; адрес registers_t В ПАМЯТИ, на собственном стеке этой же задачи:
; память (в отличие от регистра) не может быть "перезаписана" чужой
; задачей, потому что у каждой задачи свой стек. 16, а не 8 байт --
; чтобы сохранить выравнивание rsp%16==0 перед call (ABI), используя
; только младшие 8 из зарезервированных 16.
isr_common_stub:
    PUSH_ALL_REGS
    mov rax, rsp            ; rax = точный адрес начала registers_t
    and rsp, -16              ; выравниваем rsp под call
    sub rsp, 16                 ; резервируем слот в ПАМЯТИ под сохранённый адрес (16, не 8 -- ради выравнивания)
    mov [rsp], rax                ; кладём его туда, а не в регистр
    mov rdi, rax                    ; 1-й аргумент по ABI: registers_t *regs
    call isr_handler
    mov rax, [rsp]                    ; забираем обратно из памяти -- пережило что угодно
    mov rsp, rax
    POP_ALL_REGS
    add rsp, 16               ; убираем int_no и err_code
    iretq

irq_common_stub:
    PUSH_ALL_REGS
    mov rax, rsp
    and rsp, -16
    sub rsp, 16
    mov [rsp], rax
    mov rdi, rax
    call irq_handler
    mov rax, [rsp]
    mov rsp, rax
    POP_ALL_REGS
    add rsp, 16
    iretq

; ---- Системные вызовы: вектор 0x80 (128), DPL=3, см. include/idt.h ----
;
; В отличие от векторов 0-47 выше, этот гейт доступен из ring 3
; (int 0x80, выполненный обычной пользовательской программой), и именно
; поэтому здесь есть смысл говорить про "переход ring3->ring0": CPU сам
; переключает стек на TSS.RSP0 (см. kernel/gdt.c) ещё до первого push
; ниже -- но с точки зрения этого кода разницы с isr_common_stub нет
; никакой: раскладка стека (SS/RSP/RFLAGS/CS/RIP, см. include/isr.h)
; в 64-битном режиме одна и та же что при смене кольца, что без неё
; (в отличие от 32-битного protected mode, где SS/RSP кладутся только
; при реальной смене привилегий).
;
; Само число регистров общего назначения совпадает с обычным
; исключением/IRQ -- поэтому переиспользуем те же макросы и ту же
; форму стека (фиктивный код ошибки + номер вектора), чтобы
; syscall_isr_handler мог принимать тот же самый registers_t*, что и
; isr_handler/irq_handler.
global isr128
isr128:
    push qword 0
    push qword 128
    jmp syscall_common_stub

syscall_common_stub:
    PUSH_ALL_REGS
    mov rax, rsp             ; см. общий комментарий у isr_common_stub выше --
    and rsp, -16               ; тот же приём: адрес registers_t в памяти,
    sub rsp, 16                  ; а не в callee-saved регистре, потому что
    mov [rsp], rax                 ; ИМЕННО этот путь (syscall, который может
    mov rdi, rax                     ; заблокироваться и быть возобновлённым
    call syscall_isr_handler           ; ПОЗЖЕ через context_switch(), см.
    mov rax, [rsp]                       ; sys_irq_wait(), kernel/syscall.c) --
    mov rsp, rax                           ; самый долгий и глубокий вызов из
    POP_ALL_REGS                             ; всех трёх заглушек в этом файле.
    add rsp, 16
    iretq
    ; Обычный путь возврата -- для всех syscall'ов, кроме SYS_EXIT: тот
    ; вызывает process_exit() (kernel/process.c), которая помечает
    ; текущую задачу TASK_TERMINATED и вызывает task_exit() ->
    ; schedule() (kernel/task.c) -- context_switch() (kernel/context.s)
    ; уводит нас на чужой стек НАВСЕГДА, так что до "iretq" и "add rsp,16"
    ; выше для SYS_EXIT дело просто не доходит, что нормально: тот call
    ; syscall_isr_handler выше для этой задачи больше не вернётся.

; ---- Точка первого запуска ПРОЦЕССНОЙ задачи (см. kernel/task.c:
; task_create_process(), include/task.h) ----
;
; context_switch() (kernel/context.s) добирается сюда обычным "ret" --
; то есть на момент входа сюда rsp уже указывает ровно на начало
; заранее заготовленного registers_t (см. task_create_process()), как
; будто мы -- это isr_common_stub, у которого уже случился call
; isr_handler и уже выполнен "mov rsp, rbx" (восстановление точного
; адреса кадра). Поэтому дальше -- ровно тот же хвост, что и у любого
; обработчика прерывания: снять регистры общего назначения, скинуть
; фиктивные int_no/err_code и iretq -- этот iretq и есть тот самый
; переход CPL 0 -> 3, который переносит совершенно новую задачу сразу в
; ring 3, на RIP/RSP/CS/SS, заданные в этом registers_t.
global process_trampoline
process_trampoline:
    POP_ALL_REGS
    add rsp, 16
    iretq

; ---- Таблица адресов всех заглушек для idt.c ----
section .data

global isr_stub_table
isr_stub_table:
    dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    dq irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

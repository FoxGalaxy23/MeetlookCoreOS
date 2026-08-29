/* isr.c -- см. include/isr.h за общим описанием. Здесь -- таблицы
 * зарегистрированных обработчиков и сама логика диспетчеризации,
 * вызываемая из isr_common_stub/irq_common_stub (kernel/isr.s).
 */

#include "isr.h"
#include "pic.h"
#include "kprintf.h"
#include "gdt.h"
#include "paging.h"
#include "process.h"
#include "task.h"

static isr_handler_t g_exception_handlers[32];
static isr_handler_t g_irq_handlers[16];

static const char *const g_exception_names[32] = {
    "Divide-by-zero",        "Debug",                 "NMI",
    "Breakpoint",             "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",         "Device Not Available",   "Double Fault",
    "Coprocessor Segment Overrun", "Invalid TSS",       "Segment Not Present",
    "Stack-Segment Fault",    "General Protection Fault","Page Fault",
    "Reserved",                "x87 Floating-Point",     "Alignment Check",
    "Machine Check",           "SIMD Floating-Point",    "Virtualization",
    "Control Protection",      "Reserved",               "Reserved",
    "Reserved",                 "Reserved",               "Reserved",
    "Reserved",                 "Reserved",               "Hypervisor Injection",
    "VMM Communication",       "Security Exception"
};

/* #PF (vector 14) для процессных задач сначала пытается исправить
 * ситуацию (COW, рост стека/кучи -- см. include/paging.h), и только если
 * это не удалось, завершает виновный процесс -- через generic ring3-крах
 * ниже, isr_handler() до него в этом случае просто не дойдёт (обработчик
 * зарегистрирован явно, см. isr_init()). */
static void page_fault_handler(registers_t *regs) {
    u64 fault_addr = paging_read_cr2();
    task_t *cur = task_current();
    address_space_t *as = cur ? cur->as : 0;

    if (as && paging_handle_page_fault(as, fault_addr, regs->err_code)) {
        return; /* исправлено (страница подкачана либо разрешён COW) -- просто повторяем инструкцию */
    }

    if (as) {
        /* Настоящий segfault: адрес вне всех разрешённых регионов,
           попытка расширить стек/кучу за отведённый максимум, запись в
           чужую/read-only страницу без COW и т.п. -- программа сама
           виновата, убиваем только её. */
        kprintf("\n*** process crashed: page fault at 0x%x (error_code=0x%x, rip=0x%x) ***\n",
                (u32)fault_addr, (u32)regs->err_code, (u32)regs->rip);

        /* Частный, но очень распространённый случай: обращение по
           адресу из первых 4 KiB. В sandboxed-режиме (обычный 'run')
           первым аргументом (rdi) программе всегда приходит kapi=NULL
           (см. include/loader.h, kernel/process.c) -- если приложение
           написано под старый ring0-стиль (типа /bin/calc, см. его же
           справку в 'help') и обращается к kapi->что-нибудь напрямую,
           не проверив его на NULL, оно тут же падает ровно на маленьком
           смещении от нуля (адрес поля структуры kapi_t). Отличить это
           от "обычного" NULL-деref в написанной-под-syscalls программе
           средствами ядра надёжно нельзя, но подсказка тут не повредит
           ни в одном из случаев -- она лишь предлагает следующий шаг,
           не утверждая ничего категорично. */
        if (fault_addr < 0x1000) {
            kprintf("hint: crashed dereferencing a near-NULL pointer -- if this program\n"
                    "      expects direct kapi_t access (see 'help'), it needs root and\n"
                    "      'rungod <name>' instead of plain 'run'/'<name>'.\n");
        }

        process_exit(-114); /* не возвращается */
        return;              /* не достигается */
    }

    /* as == 0 -- это поток ядра (в т.ч. god-режим, kernel/loader.c:
       loader_run_file_god() -- у него нет собственного адресного
       пространства и никогда не было своих проверок памяти), т.е.
       практически наверняка баг самого ядра. Печатаем диагностику и
       останавливаемся -- см. общий путь ниже, isr_handler() дойдёт до
       него сам, если этот обработчик просто вернёт управление, ничего
       не предприняв, поэтому явно falls through в generic-путь. */
    kprintf("\n*** UNHANDLED PAGE FAULT in kernel/ring0 context ***\n");
    kprintf("  fault_addr=0x%x  error_code=0x%x  rip=0x%x\n",
            (u32)fault_addr, (u32)regs->err_code, (u32)regs->rip);
    kprintf("System halted.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void isr_init(void) {
    isr_register_handler(14, page_fault_handler);
}

void isr_register_handler(u8 num, isr_handler_t handler) {
    if (num < 32) g_exception_handlers[num] = handler;
}

void irq_register_handler(u8 irq, isr_handler_t handler) {
    if (irq < 16) g_irq_handlers[irq] = handler;
}

bool_t irq_handler_is_registered(u8 irq) {
    return irq < 16 && g_irq_handlers[irq] != 0;
}

void isr_handler(registers_t *regs) {
    u64 n = regs->int_no;

    if (n < 32 && g_exception_handlers[n] != 0) {
        g_exception_handlers[n](regs);
        return;
    }

    /* Исключение (кроме #PF -- у него свой обработчик выше, см.
       page_fault_handler()), случившееся в ring 3 внутри настоящего
       процесса (task_current()->as != 0, см. include/task.h,
       include/process.h) -- это, скорее всего, обычная ошибка ЭТОЙ
       программы (деление на ноль, недопустимая инструкция и т.п.), а не
       баг ядра. Останавливать всю систему из-за чужой ошибки было бы
       ровно тем самым результатом, ради предотвращения которого и
       добавлялись GDT/TSS/ring3/paging -- вместо этого печатаем
       диагностику и завершаем ТОЛЬКО эту задачу (process_exit(), не
       возвращается), а не всю систему. */
    {
        task_t *cur = task_current();
        if ((regs->cs & 3) == 3 && cur != 0 && cur->as != 0) {
            kprintf("\n*** sandboxed program crashed: %s",
                    (n < 32) ? g_exception_names[n] : "unknown exception");
            kprintf(" (vector %u, error_code=0x%x, rip=0x%x) ***\n",
                    (u32)n, (u32)regs->err_code, (u32)regs->rip);
            process_exit((s32)(-100 - (s32)n)); /* не возвращается */
            return; /* не достигается */
        }
    }

    /* Необработанное исключение процессора в ring 0 -- почти всегда баг
       самого ядра. Безопаснее всего честно остановиться с диагностикой,
       а не пытаться "прожить" дальше со сломанным состоянием -- тот же
       путь, что уже используется в power.c для фатальных ситуаций.
       Адреса печатаются как 32 бита: у ядра и всех его структур адреса
       заведомо ниже 4 GiB (см. boot/boot.s). */
    kprintf("\n*** UNHANDLED CPU EXCEPTION %u", (u32)n);
    if (n < 32) kprintf(" (%s)", g_exception_names[n]);
    kprintf(" ***\n");
    kprintf("  error_code=0x%x  rip=0x%x  cs=0x%x  rflags=0x%x\n",
            (u32)regs->err_code, (u32)regs->rip, (u32)regs->cs, (u32)regs->rflags);
    kprintf("  rsp=0x%x  ss=0x%x\n", (u32)regs->rsp, (u32)regs->ss);
    kprintf("System halted.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void irq_handler(registers_t *regs) {
    u8 irq = (u8)(regs->int_no - 32);

    /* EOI отправляется ДО вызова конкретного обработчика, а не после.
       Обработчик таймера (см. timer.c) может через scheduler_tick()
       выполнить переключение задачи и не "вернуться" сюда обычным
       образом ещё много миллисекунд (пока планировщик снова не выберет
       именно этот момент выполнения) -- откладывать EOI до того момента
       означало бы держать PIC в неведении и заблокировать вообще все
       IRQ с этим номером и выше на всё это время. */
    pic_send_eoi(irq);

    if (irq < 16 && g_irq_handlers[irq] != 0) {
        g_irq_handlers[irq](regs);
    }
}

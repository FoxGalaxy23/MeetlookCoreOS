/* process.c -- см. include/process.h за общей идеей. */

#include "process.h"
#include "task.h"
#include "paging.h"
#include "gdt.h"
#include "loader.h"
#include "memlayout.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "ipc.h"
#include "container.h"

/* Раскладывает argv[]+строки на ВЕРШИНЕ стека процесса -- в последней
   (верхней) странице стекового окна [PROC_STACK_BASE, PROC_STACK_TOP).
   execve() в настоящих ОС делает то же самое: аргументы кладутся прямо
   под «настоящий» начальный RSP, чтобы стек рос вниз, не затрагивая их.
   Страница выделяется и отображается сразу (не лениво), потому что нам
   нужно туда записать данные ДО первого шага процесса. */
#define ARGV_AREA_SIZE 4096u
/* Виртуальный адрес страницы argv: последняя страница стекового окна. */
#define ARGV_PAGE_VADDR (PROC_STACK_BASE + PROC_STACK_SIZE - ARGV_AREA_SIZE)

static bool_t build_argv(address_space_t *as, u32 argc, char **argv, u64 *out_argv_vaddr) {
    if (argc == 0) { *out_argv_vaddr = 0; return TRUE; }

    /* executable=FALSE -- страница argv лежит внутри стекового окна и
       содержит только данные (строки аргументов), см. W^X-комментарий в
       include/paging.h: писать туда программе можно (в принципе), а вот
       исполнять оттуда -- никогда не требуется в норме и ровно то, что
       нужно было бы эксплойту, подделавшему аргументы. */
    void *frame = as_alloc_and_map(as, ARGV_PAGE_VADDR, TRUE, FALSE);
    if (!frame) return FALSE;

    u8 *area = (u8 *)frame; /* identity-mapped -- пишем напрямую */
    u32 offset = (u32)(argc + 1) * (u32)sizeof(char *);
    if (offset > ARGV_AREA_SIZE) return FALSE;

    char **uargv = (char **)area;
    for (u32 i = 0; i < argc; i++) {
        u32 len = strlen(argv[i]) + 1;
        if (offset + len > ARGV_AREA_SIZE) return FALSE;
        memcpy(area + offset, argv[i], len);
        uargv[i] = (char *)(uintptr_t)(ARGV_PAGE_VADDR + offset);
        offset += len;
    }
    uargv[argc] = 0;

    *out_argv_vaddr = ARGV_PAGE_VADDR;
    return TRUE;
}

s32 process_run_sandboxed(const void *elf_data, u32 elf_size, u32 argc, char **argv) {
    /* Без контейнера -- полное доверие, как и раньше (обратная
       совместимость: 'run' в shell/shell.c и т.п.); io_base/io_len=0,
       irq_line=CONTAINER_IRQ_NONE -- у полностью доверенного 'run' и
       так есть syscall'ы на всё, что нужно, отдельного прямого доступа
       к портам/IRQ ему не требуется. */
    return process_run_sandboxed_caps(elf_data, elf_size, argc, argv, CAP_ALL, 0, 0, CONTAINER_IRQ_NONE);
}

s32 process_run_sandboxed_caps(const void *elf_data, u32 elf_size, u32 argc, char **argv,
                                u32 caps, u16 io_base, u16 io_len, u8 irq_line) {
    address_space_t *as = as_create();
    if (!as) {
        kprintf("process: out of memory (address space)\n");
        return -1;
    }

    u64 entry_point = elf_load(elf_data, elf_size, as);
    if (entry_point == 0) {
        as_destroy(as);
        kprintf("process: failed to load ELF binary\n");
        return -1;
    }

    u64 argv_vaddr = 0;
    if (!build_argv(as, argc, argv, &argv_vaddr)) {
        as_destroy(as);
        kprintf("process: too many/too long arguments for a sandboxed program\n");
        return -1;
    }

    registers_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip    = entry_point;       /* точка входа -- из ELF */
    regs.cs     = GDT_USER_CODE_SEL;
    regs.rflags = 0x202;             /* IF=1 + зарезервированный бит 1 */
    /* RSP: начало под страницей argv (которая в конце стекового окна).
       ВАЖНО: наш вход в app_main -- это iretq, а не call, так что в
       отличие от настоящего вызова функции адрес возврата НЕ проталкивается
       на стек. System V AMD64 ABI, тем не менее, требует RSP % 16 == 0
       именно перед call, а значит ВНУТРИ вызванной функции (сразу после
       того, как call сам протолкнул 8 байт адреса возврата) её "родной"
       RSP всегда на 8 меньше кратного 16 -- и любой корректно
       откомпилированный пролог (push rbp, sub rsp, N с N ≡ 8 (mod 16) и
       т.п.) рассчитан ИМЕННО на этот сдвиг, а не на честно выровненный
       по 16 старт. Раньше здесь стоял честный '& ~0xFULL' без поправки
       -- "для простоты", как гласил старый комментарий -- но это не
       просто теоретическое отступление от ABI: GCC на -O2 при случае
       сам, без всякого явного SSE/intrinsics в исходнике, генерирует
       выровненные по 16 байт SSE-обращения к стеку (movaps и т.п.) --
       например, ради инициализации массива указателей одним 16-байтным
       движением вместо двух 8-байтных. Такая инструкция ТРЕБУЕТ
       настоящего 16-байтного выравнивания адреса и получает #GP, если
       его нет -- ровно так падал /bin/exectest (см. apps/exectest.c:
       локальный массив exec_argv[4] компилятор проинициализировал
       через movaps). Поэтому здесь нужно на 8 байт МЕНЬШЕ выровненного
       по 16, а не выровненное по 16 значение как таковое. */
    regs.rsp    = ((ARGV_PAGE_VADDR - 16) & ~0xFULL) - 8;
    regs.ss     = GDT_USER_DATA_SEL;
    regs.rdi    = 0;                 /* app_main(kapi=NULL, ...) -- в ring 3 kapi_t недоступен */
    regs.rsi    = argc;
    regs.rdx    = argv_vaddr;

    task_t *t = task_create_process("app", as, &regs);
    if (!t) {
        as_destroy(as);
        kprintf("process: out of memory (task)\n");
        return -1;
    }

    t->caps = caps;
    t->io_port_base = io_base;
    t->io_port_len = io_len;
    t->irq_line = irq_line;

    /* Мы держим указатель на t и сами прочитаем t->exit_code ниже --
       выставляем has_waiter, чтобы автоматический сборщик зомби внутри
       schedule() не освободил task_t/стек у нас из-под ног раньше
       времени (см. include/task.h, kernel/task.c:reap_terminated_from).
       Если он вдруг форкнётся (see process_fork() ниже), ЕГО потомок
       такого флага не получает и будет реапнут автоматически, как только
       завершится -- у него никогда не будет своего "process_run_sandboxed",
       который бы за ним следил. */
    t->has_waiter = TRUE;

    /* Ждём завершения ИМЕННО этого процесса, отдавая процессор всем
       остальным -- сам вызывающий (shell) остаётся обычной задачей в
       очереди, и планировщик как обычно крутит round-robin между ним,
       демо-задачами и, теперь, самим процессом (и его потомками, если
       он успеет вызвать fork() -- см. process_fork() ниже). */
    while (t->state != TASK_TERMINATED) {
        task_yield();
    }

    s32 code = t->exit_code;
    task_reap(t); /* t недействителен сразу после этого вызова */
    return code;
}

void process_exit(s32 code) {
    task_current()->exit_code = code;
    task_exit(); /* не возвращается */
    for (;;) { __asm__ volatile ("cli; hlt"); } /* не достигается */
}

s64 process_fork(const registers_t *regs) {
    task_t *cur = task_current();
    if (!cur->as) return -1; /* fork() имеет смысл только для настоящего процесса */

    address_space_t *child_as = as_clone_cow(cur->as);
    if (!child_as) return -1;

    registers_t child_regs = *regs;
    child_regs.rax = 0; /* потомок увидит fork() == 0 -- см. include/process.h */

    task_t *child = task_create_process(cur->name, child_as, &child_regs);
    if (!child) {
        as_destroy(child_as);
        return -1;
    }

    child->caps = cur->caps; /* fork() не расширяет права -- см. include/task.h::caps */
    child->io_port_base = cur->io_port_base;
    child->io_port_len = cur->io_port_len;
    child->irq_line = cur->irq_line;

    /* Помечаем родство и держим ребёнка "зомби" до явного SYS_WAITPID
       (см. process_waitpid() ниже и task_t::parent/has_waiter в
       include/task.h) -- ровно как у прямого потомка
       process_run_sandboxed(), только читать exit_code и звать
       task_reap() теперь может САМ пользовательский код через
       SYS_WAITPID, а не обязательно ядро. Если cur завершится, так и не
       дождавшись этого ребёнка -- task_exit()/orphan_children() снимет
       оба этих поля сама, и ребёнка заберёт обычный автосборщик (см.
       комментарий там же). */
    child->parent = cur;
    child->has_waiter = TRUE;

    /* Разделяемая память уже физически общая (as_clone_cow() выше не
       трогает shm-слот через COW, см. её комментарий в kernel/paging.c)
       -- здесь копируем саму таблицу хендлов и досчитываем держателей
       пайпов/очередей/attach_count shm-сегментов, чтобы потомок стал
       ПОЛНОПРАВНЫМ дополнительным держателем каждого унаследованного
       объекта, а не просто получил копию числа без реального смысла за
       ним (см. include/ipc.h за подробностями). */
    ipc_task_fork_copy(cur, child);

    return (s64)child->id;
}

/* SYS_WAITPID (см. include/sysabi.h). pid < 0 -- подождать ЛЮБОГО прямого
 * потомка, порождённого нашим process_fork(); pid >= 0 -- подождать
 * конкретно задачу с этим id (и только если она действительно наш
 * ребёнок -- чужой pid, как и в POSIX, воспринимается как "нет такого
 * потомка", а не как разрешение подглядывать за чужими детьми).
 *
 * Реализовано в том же кооперативном стиле, что и ожидание в
 * process_run_sandboxed() выше: раз система однопроцессорная и
 * планировщик всё равно вытесняющий (см. kernel/task.c), простой цикл
 * "проверить -- иначе отдать процессор" корректен и не жжёт CPU впустую
 * ровно так же, как обычная блокировка -- task_yield() не возвращает
 * управление вызывающему, пока другим READY-задачам (в т.ч. как раз
 * тому ребёнку, которого мы ждём) не дали поработать. Настоящая
 * waitqueue-based блокировка (без опроса) -- разумное следующее
 * улучшение, но не обязательное для корректности.
 *
 * out_code (если не 0) получает exit_code найденного потомка. Возвращает
 * id завершившегося потомка либо -1, если у нас вообще нет (или больше
 * нет) ни одного потомка, подходящего под pid -- аналог ECHILD. */
s64 process_waitpid(s32 pid, s32 *out_code) {
    task_t *cur = task_current();

    for (;;) {
        task_t *child = 0;
        wait_child_result_t r = task_find_child(cur, pid, &child);

        if (r == WAIT_CHILD_NONE) {
            return -1; /* нет такого потомка вообще -- ждать нечего */
        }

        if (r == WAIT_CHILD_DONE) {
            s32 code = child->exit_code;
            u32 cid = child->id;
            task_reap(child); /* child недействителен сразу после этого вызова */
            if (out_code) *out_code = code;
            return (s64)cid;
        }

        /* WAIT_CHILD_ALIVE: подходящий потомок есть, но ещё выполняется --
           отдаём процессор и проверяем снова на следующем витке. */
        task_yield();
    }
}

s64 process_execve(const void *elf_data, u32 elf_size, u32 argc, char **argv, registers_t *regs) {
    address_space_t *as_new = as_create();
    if (!as_new) return -1;

    u64 entry_point = elf_load(elf_data, elf_size, as_new);
    if (entry_point == 0) {
        as_destroy(as_new);
        return -1;
    }

    u64 argv_vaddr = 0;
    if (!build_argv(as_new, argc, argv, &argv_vaddr)) {
        as_destroy(as_new);
        return -1;
    }

    task_t *cur = task_current();
    address_space_t *as_old = cur->as;

    /* Освобождаем IPC-ресурсы старого процесса ДО смены адресного
       пространства: ipc_task_cleanup() трогает только бухгалтерию
       самих объектов (attach_count пайпов/очередей/shm), а не PTE --
       физические кадры освободит as_destroy() ниже. Без этого шага
       хендлы в cur->ipc_handles остались бы «висеть» в новом образе,
       занимая пайпы/очереди/shm-счётчики. */
    if (!cur->ipc_freed) {
        ipc_task_cleanup(cur);
        cur->ipc_freed = TRUE;
    }

    /* Переключаемся на новое адресное пространство. После этого ядро
       продолжает видеть всю физическую память через identity map, поэтому
       as_destroy(as_old) ниже по-прежнему работает корректно -- он
       обращается к структурам as_old через физические адреса в куче
       ядра, не через per-process PTE. */
    paging_switch(as_new);
    cur->as = as_new;

    /* Теперь безопасно уничтожить старые страничные таблицы и кадры. */
    as_destroy(as_old);

    /* Настраиваем регистры: ISR вернётся по ним в ring 3 через iretq. */
    regs->rip    = entry_point;
    /* RSP: прямо под страницей argv (в конце стекового окна). -8 от
       выровненного по 16 -- та же поправка, что и в process_run_sandboxed()
       выше (см. подробный комментарий там): вход через iretq не толкает
       адрес возврата, как это сделал бы call, поэтому нужно явно
       сымитировать тот самый сдвиг на 8 байт, который ожидает пролог
       любой честно откомпилированной по System V AMD64 ABI функции. */
    regs->rsp    = ((ARGV_PAGE_VADDR - 16) & ~0xFULL) - 8;
    regs->rdi    = 0;   /* kapi=NULL в ring 3 */
    regs->rsi    = (u64)argc;
    regs->rdx    = argv_vaddr;

    /* Обнуляем остальные GPR для предсказуемого состояния. */
    regs->rax = 0;
    regs->rbx = 0;
    regs->rcx = 0;
    regs->rbp = 0;
    regs->r8  = 0;
    regs->r9  = 0;
    regs->r10 = 0;
    regs->r11 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;

    return 0;
}

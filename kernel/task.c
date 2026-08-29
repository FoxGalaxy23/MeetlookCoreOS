/* task.c -- см. include/task.h за общим описанием модели.
 *
 * Как устроено переключение "физически": каждая задача -- это
 * task_t + собственный стек. Переключение делает context_switch()
 * (kernel/context.s), сохраняя/восстанавливая только callee-saved
 * регистры и rsp; RIP восстанавливается неявно через ret. Подробности
 * раскладки начального стека новой задачи см. в комментариях к
 * task_create() ниже -- она обязана точно соответствовать порядку
 * push/pop в context.s.
 */

#include "task.h"
#include "context.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "gdt.h"
#include "paging.h"
#include "timer.h"
#include "ipc.h"
#include "spinlock.h"
#include "percpu.h"

/* ---- SMP: от одного g_current к per-CPU this_cpu()->current_task ----
 *
 * До SMP (см. include/smp.h за общей картиной) в системе было ровно
 * одно исполняемое ядро, и "текущая задача" была одним-единственным
 * глобальным указателем. Теперь у КАЖДОГО физического ядра своя
 * текущая задача (this_cpu()->current_task, см. include/percpu.h) --
 * макрос ниже позволяет не переписывать все места, где раньше стояло
 * g_current, вручную: g_current остаётся ЛЕКСИЧЕСКИ тем же самым
 * l-value (можно читать И присваивать), просто теперь означает "текущая
   задача ТОГО ядра, которое выполняет этот код прямо сейчас", а не
   единственную задачу на всю машину. */
#define g_current (this_cpu()->current_task)

/* Кольцо задач, g_task_count и g_next_id остаются ОБЩИМИ на все ядра
 * (единая run queue, см. общий комментарий в include/smp.h) -- отсюда
 * и необходимость в g_sched_lock ниже: раньше единственной защитой было
 * cli/sti (см. save_and_disable_interrupts()), этого хватало, пока
 * существовал только один процессор, способный вообще читать/писать
 * эти структуры. Теперь два ядра могут одновременно попасть в
 * schedule()/task_create()/task_reap() и без настоящего спинлока
 * (include/spinlock.h) их одновременная работа со списком/счётчиками
 * гарантированно повредила бы кольцо (классическая гонка на связном
 * списке). ВАЖНО: лок обязан отпускаться ДО фактического
 * context_switch() (см. schedule() ниже), а не после -- иначе задача,
 * приостановленная "с локом в кармане", заблокировала бы им же вообще
 * все остальные ядра до тех пор, пока её саму кто-то не выберет
 * снова -- а выбрать её может только тот, кому нужен ровно этот же лок,
 * классический deadlock. См. подробный комментарий над schedule(). */
static spinlock_t g_sched_lock = SPINLOCK_INIT;

/* Опорная точка для присоединения новых узлов к общему кольцу, когда у
 * присоединяющегося ядра ЕЩЁ НЕТ собственной текущей задачи, к которой
 * можно прицепиться через g_current->next (см. tasking_init() -- он же
 * и заполняет этот указатель для самого первого узла, task_register_this_cpu()
 * ниже использует его для узлов AP). Сам по себе не имеет отношения к
 * планированию -- обычный указатель "хоть какой-то живой узел кольца",
 * защищённый тем же g_sched_lock, что и всё остальное кольцо. */
static task_t *g_any_task = 0;

/* Точка входа новой ПРОЦЕССНОЙ задачи -- см. kernel/isr.s. Не обычная
   C-функция: ожидает, что context_switch() только что вернул её на
   стек, где сразу после места, куда указывает rsp, лежит полный
   registers_t (см. task_create_process() ниже), и доигрывает то же
   самое (POP_ALL_REGS + iretq), что обычный обработчик прерывания делал
   бы, "возвращаясь" из ring 0 в ring 3. */
extern void process_trampoline(void);

static u32 g_task_count = 0;
static u32 g_next_id = 0;

/* Критическая секция: пока мы читаем/меняем состояние задач и (возможно)
 * выполняем context_switch, нельзя допустить повторного входа сюда же по
 * таймеру -- иначе список задач и поля вроде state/switches будут
 * повреждены гонкой. Сохраняем и восстанавливаем именно ИСХОДНЫЙ флаг
 * IF, а не жёстко ставим sti: если schedule() вызвана из таймерного IRQ
 * (scheduler_tick(), где IF и так уже 0 -- CPU сам сбросил его на входе
 * в interrupt gate), после возврата сюда (когда таймер снова выберет
 * именно эту задачу) IF обязан остаться 0 ровно до настоящего iretq в
 * isr.s -- поднимать его раньше означало бы врать процессору о том, что
 * мы уже вышли из обработчика прерывания. */
static u64 save_and_disable_interrupts(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void restore_interrupts(u64 flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* Освобождает ресурсы уже завершившихся задач -- безопасно делать это
 * только для задач, которые точно не являются prev (текущей, ещё
 * выполняющейся на своём стеке ПРЯМО СЕЙЧАС) -- иначе можно было бы
 * попытаться разрушить набор таблиц страниц или стек, которые в этот
 * самый момент ещё активны. К моменту, когда какая-то ДРУГАЯ задача
 * вызовет schedule() и увидит данную задачу TERMINATED, эта задача уже
 * гарантированно была переключена на что-то ещё хотя бы один раз -- то
 * есть ни её as, ни её стек прямо сейчас не используются процессором.
 *
 * Адресное пространство (as) освобождается для ЛЮБОЙ TERMINATED-задачи
 * сразу -- эта память ей больше никогда не понадобится, независимо от
 * того, интересуется ли кто-то её exit_code.
 *
 * А вот сам task_t и его стек ядра освобождаются (и задача отвязывается
 * от кольцевого списка) только если has_waiter == FALSE, т.е. никто не
 * держит указатель на неё, чтобы прочитать exit_code -- см. комментарий
 * к has_waiter в include/task.h. Если has_waiter == TRUE (сейчас это
 * только прямой ребёнок process_run_sandboxed()), задачу оставляем как
 * есть: её явно освободит task_reap() ПОСЛЕ того, как вызывающий прочитал
 * exit_code -- досрочное освобождение отсюда было бы classic
 * use-after-free гонкой с тем чтением. */
static void reap_terminated_from(task_t *skip) {
    task_t *prev_node = skip;
    task_t *t = skip->next;
    while (t != skip) {
        task_t *next = t->next;

        if (t->state == TASK_TERMINATED) {
            if (!t->ipc_freed) {
                /* Закрывает/отсоединяет все пайпы/очереди/shm-сегменты,
                   к которым t ещё был подключён (см. include/ipc.h) --
                   ДО as_destroy() ниже, хотя порядок здесь на самом деле
                   не важен для shm: ipc_task_cleanup() трогает только
                   бухгалтерию самих IPC-объектов (attach_count и т.п.),
                   а сами PTE/физические кадры процессного окна (включая
                   shm-слот) освобождает уже as_destroy() ниже, единым
                   проходом, как и для COW-кадров. */
                ipc_task_cleanup(t);
                t->ipc_freed = TRUE;
            }
            if (t->as != 0 && !t->as_freed) {
                as_destroy(t->as);
                t->as = 0;
                t->as_freed = TRUE;
            }
            if (!t->has_waiter) {
                prev_node->next = next; /* отвязываем t из кольца */
                kfree(t->stack_base);
                kfree(t);
                g_task_count--;
                t = next;
                continue;
            }
        }

        prev_node = t;
        t = next;
    }
}

/* Явное освобождение зомби-задачи, за которой кто-то следил через
 * has_waiter (см. include/task.h). Вызывающий уже прочитал всё нужное
 * (exit_code) и гарантирует, что t->state == TASK_TERMINATED -- в
 * противном случае это баг вызывающего кода, а не сценарий, который
 * стоит пытаться "мягко" обработать. Отключаем прерывания на время
 * отвязывания из кольцевого списка -- иначе таймерный тик мог бы влезть
 * посреди обхода (см. schedule()/reap_terminated_from) и повредить
 * список. */
void task_reap(task_t *t) {
    if (!t) return;

    u64 saved_flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    if (!t->ipc_freed) {
        ipc_task_cleanup(t);
        t->ipc_freed = TRUE;
    }
    if (t->as != 0 && !t->as_freed) {
        as_destroy(t->as);
        t->as = 0;
        t->as_freed = TRUE;
    }

    task_t *p = g_current;
    while (p->next != t) p = p->next; /* t гарантированно ещё в кольце */
    p->next = t->next;
    g_task_count--;

    kfree(t->stack_base);
    kfree(t);

    spin_unlock(&g_sched_lock);
    restore_interrupts(saved_flags);
}

/* ВАЖНО про SMP (см. общий комментарий у g_sched_lock выше): лок
 * захватывается вместе с cli (save_and_disable_interrupts()) на время
 * ПОИСКА кандидата и правки состояния/списка, но ОТПУСКАЕТСЯ ещё ДО
 * вызова context_switch() -- то есть до того, как этот CPU физически
 * перестанет выполнять код prev. Если бы лок оставался захваченным через
 * весь context_switch (как можно было бы наивно предположить, скопировав
 * старую cli-модель "выключили прерывания на всё время"), возник бы
 * настоящий deadlock: другое ядро, которому нужно перепланировать СВОЮ
 * задачу, обязано взять тот же самый g_sched_lock -- но взять его
 * получится только тогда, когда prev (держащая лок) снова получит
 * управление и дойдёт до конца этой функции, а получить управление
 * снова prev способна только через чей-то chselect (в т.ч. потенциально
 * того самого заблокированного ядра). Отпуская лок ДО переключения, мы
 * оставляем candidate уже помеченным TASK_RUNNING (см. ниже) -- этого
 * достаточно, чтобы никакое другое ядро не выбрало его повторно, даже
 * если реальный context_switch на candidate ещё не успел физически
 * начаться на этом CPU.
 *
 * Восстановление флага IF (в отличие от лока) по-прежнему происходит
 * ПОСЛЕ context_switch(), ровно как в исходной однопроцессорной версии
 * -- IF prev сохранён в её собственном стековом кадре (saved_flags -- это
 * локальная переменная), поэтому она переживает миграцию prev на другое
 * физическое ядро между приостановкой и возобновлением: когда бы и на
 * каком бы CPU prev ни "вернулась" из context_switch(), это по-прежнему
 * тот же самый стековый кадр с тем же самым saved_flags. */
static void schedule(void) {
    u64 saved_flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    task_t *prev = g_current;
    reap_terminated_from(prev);
    task_t *next = prev->next;

    /* Ищем по кругу ближайшую задачу, готовую к выполнению, пропуская
       завершённые, заблокированные -- и (это НОВОЕ по сравнению с
       однопроцессорной версией) задачи, которые прямо сейчас TASK_RUNNING
       на КАКОМ-ТО ДРУГОМ ядре: раньше TASK_RUNNING могла означать только
       "это prev, других вариантов физически не было", теперь же в один
       момент времени TASK_RUNNING может быть сразу несколько разных
       задач -- по одной на каждое занятое ядро -- и попытка выбрать
       чужую RUNNING-задачу означала бы запустить один и тот же task_t
       (один и тот же стек!) сразу на двух ядрах одновременно. */
    task_t *candidate = next;
    while (candidate->state == TASK_TERMINATED
           || candidate->state == TASK_BLOCKED
           || (candidate->state == TASK_RUNNING && candidate != prev)) {
        candidate = candidate->next;
        if (candidate == next) break;
    }

    if (candidate->state == TASK_TERMINATED || candidate->state == TASK_BLOCKED
        || (candidate->state == TASK_RUNNING && candidate != prev)) {
        if (prev->state == TASK_TERMINATED) {
            /* На однопроцессорной системе это означало бы "вообще
               никаких живых задач в системе" -- на SMP это означает
               только "нечем заняться ИМЕННО этому ядру" (у других ядер
               могут быть свои задачи) -- останавливать всю машину было
               бы неправильно, но в этой системе у каждого ядра всегда
               есть собственная не-завершающаяся idle-задача (см.
               ap_idle_entry() в kernel/smp.c и "задачу 0" BSP,
               создаваемую tasking_init()), так что этот путь практике
               не достижим -- оставлен как диагностика на случай бага. */
            kprintf("\nscheduler: cpu %u has no runnable task, halting this core.\n",
                    this_cpu()->cpu_index);
            spin_unlock(&g_sched_lock);
            for (;;) { __asm__ volatile ("cli; hlt"); }
        }
        prev->state = TASK_RUNNING;
        spin_unlock(&g_sched_lock);
        restore_interrupts(saved_flags);
        return; /* единственная задача, которую может выполнять ЭТО ядро прямо сейчас -- это prev */
    }

    if (candidate == prev) {
        prev->state = TASK_RUNNING;
        spin_unlock(&g_sched_lock);
        restore_interrupts(saved_flags);
        return;
    }

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    candidate->state = TASK_RUNNING;
    candidate->switches++;
    g_current = candidate;

    /* Если candidate -- настоящий процесс (собственное адресное
       пространство), она может в любой момент оказаться в ring 3 --
       CR3 обязан указывать на её таблицы страниц, а TSS.RSP0 -- на её
       СОБСТВЕННЫЙ стек ядра, чтобы следующее прерывание/исключение из
       ring 3 (в т.ч. тот самый iretq ниже, если задача запускается
       впервые через process_trampoline, см. kernel/isr.s) приземлилось
       именно туда, а не затоптало чей-то чужой стек. Для потоков ядра
       (as == 0) ни то, ни другое не нужно -- они в ring 3 не попадают.
       gdt_set_kernel_stack()/paging_switch() трогают ТОЛЬКО состояние
       ЭТОГО физического ядра (свой TSS, свой CR3, см. include/percpu.h) --
       безопасны без лока, другое ядро их не видит и не трогает. */
    if (candidate->as != 0) {
        gdt_set_kernel_stack((u64)(uintptr_t)candidate->stack_base + candidate->stack_size);
        gdt_apply_io_permissions(candidate->io_port_base, candidate->io_port_len);
    }
    paging_switch(candidate->as);

    /* Отпускаем лок ЗДЕСЬ -- см. общий комментарий над функцией. */
    spin_unlock(&g_sched_lock);

    context_switch(&prev->rsp, candidate->rsp);
    /* Когда мы снова вернёмся в ЭТУ строчку (то есть снова выберут
       "prev" в качестве текущей задачи -- возможно, на ДРУГОМ физическом
       ядре, планировщик не привязывает задачи к конкретному CPU),
       выполнение продолжится прямо отсюда -- context_switch() в этом
       смысле ведёт себя как обычный вызов функции, который "долго не
       возвращался". saved_flags здесь -- это флаги ИМЕННО prev на
       момент, когда она сама вызвала schedule(), поэтому восстанавливаем
       их только сейчас. */
    restore_interrupts(saved_flags);
}

void tasking_init(void) {
    task_t *t0 = (task_t *)kmalloc(sizeof(task_t));
    memset(t0, 0, sizeof(task_t));
    t0->id = g_next_id++;
    strncpy(t0->name, "kernel", TASK_NAME_MAX - 1);
    t0->state = TASK_RUNNING;
    t0->rsp = 0;              /* неактуально, пока задача сама выполняется */
    t0->stack_base = 0;        /* использует исходный стек ядра из boot.s, им не владеет */
    t0->stack_size = 0;
    t0->next = t0;             /* кольцо из одного элемента */

    g_current = t0;
    g_task_count = 1;
    g_any_task = t0; /* опорная точка для task_register_this_cpu() каждого будущего AP, см. include/smp.h */
}

/* См. include/task.h за общим контрактом. Аналог tasking_init(), но для
   AP: вместо создания НОВОГО кольца из одного элемента, присоединяет
   узел к УЖЕ существующему общему кольцу (см. g_any_task выше) -- узел
   гарантированно попадёт в READY-обход других ядер (в т.ч. потенциально
   ещё до того, как мы сами пометим его RUNNING, если кто-то успеет
   заглянуть в кольцо между вставкой и этой пометкой) -- поэтому вставка,
   пометка RUNNING и присвоение g_current выполняются одним атомарным (по
   отношению к остальным ядрам) шагом под g_sched_lock. */
void task_register_this_cpu(const char *name) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(task_t));
    t->id = 0; /* переприсвоим под локом ниже, вместе с g_next_id -- см. далее */
    strncpy(t->name, name ? name : "cpu", TASK_NAME_MAX - 1);
    t->state = TASK_RUNNING;
    t->rsp = 0;
    t->stack_base = 0; /* использует собственный стек, на котором ap_entry64() уже выполняется */
    t->stack_size = 0;

    u64 flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    t->id = g_next_id++;
    t->next = g_any_task->next;
    g_any_task->next = t;
    g_task_count++;

    spin_unlock(&g_sched_lock);
    restore_interrupts(flags);

    g_current = t; /* this_cpu()->current_task -- см. макрос g_current выше; это чисто
                       локальная для ДАННОГО ядра запись, отдельного лока не требует */
}

/* Вызывается из task_trampoline (context.s) при первом запуске новой
   задачи. entry(arg) -- обычный вызов её точки входа; если entry
   когда-нибудь вернётся (а не зациклится сама), задачу нужно аккуратно
   завершить, а не провалиться в случайную память после неё. */
void task_entry_wrapper(void *arg, task_entry_t entry) {
    /* Новая задача попадает сюда напрямую из context_switch() (через
       task_trampoline, см. kernel/context.s), а не через "восстановление
       флагов" в конце schedule() -- то есть IF унаследован от состояния
       CPU в момент, когда её выбрали (внутри критической секции
       планировщика, где прерывания намеренно выключены, см. schedule()).
       Явно включаем прерывания здесь: без этого свежесозданная задача
       навсегда осталась бы работать с cli, и её саму больше никогда не
       смог бы вытеснить таймер. */
    __asm__ volatile ("sti");
    entry(arg);
    task_exit();
    /* Не достигается: task_exit() переключает на другую задачу и не
       возвращается. Оставлено на случай логической ошибки выше. */
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

task_t *task_create(const char *name, task_entry_t entry, void *arg) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return 0;
    memset(t, 0, sizeof(task_t));

    u8 *stack = (u8 *)kmalloc(TASK_STACK_SIZE);
    if (!stack) { kfree(t); return 0; }

    t->id = g_next_id++;
    strncpy(t->name, name ? name : "task", TASK_NAME_MAX - 1);
    t->state = TASK_READY;
    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->switches = 0;

    /* Строим начальный контекст на вершине стека задачи. Стек растёт
       вниз, поэтому "вершина" -- это старший адрес, выровненный по 16
       байт (так context_switch() в итоге передаёт управление в
       task_trampoline() с корректно выровненным по ABI rsp, см.
       kernel/context.s). Дальше кладём в обратном порядке ровно то,
       что context_switch() ожидает снять со стека своими pop'ами:
       r15, r14, r13, r12, rbx, rbp, а поверх всего -- "обратный адрес"
       (task_trampoline), который заберёт себе ret. r12/r13 намеренно
       используются как переносчики entry/arg -- после pop'ов они
       окажутся в настоящих регистрах r12/r13, откуда их и забирает
       сам task_trampoline (см. context.s). */
    u64 *sp = (u64 *)((u64)(stack + TASK_STACK_SIZE) & ~0xFULL);

    *--sp = (u64)task_trampoline; /* "адрес возврата" для ret в context_switch */
    *--sp = 0;                     /* rbp */
    *--sp = 0;                     /* rbx */
    *--sp = (u64)entry;            /* r12 -- будет прочитан task_trampoline как entry */
    *--sp = (u64)arg;              /* r13 -- будет прочитан task_trampoline как arg */
    *--sp = 0;                     /* r14 */
    *--sp = 0;                     /* r15 */

    t->rsp = (u64)sp;

    /* Вставляем в кольцевой список сразу после текущей задачи. Список --
       общая на все ядра структура (см. общий комментарий у g_sched_lock),
       поэтому вставка обязана быть под локом: конкурентный schedule() на
       другом ядре в это же время обходит t->next по всему кольцу. */
    u64 flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);
    t->next = g_current->next;
    g_current->next = t;
    g_task_count++;
    spin_unlock(&g_sched_lock);
    restore_interrupts(flags);

    return t;
}

task_t *task_create_process(const char *name, address_space_t *as, const registers_t *regs) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return 0;
    memset(t, 0, sizeof(task_t));

    u8 *stack = (u8 *)kmalloc(TASK_STACK_SIZE);
    if (!stack) { kfree(t); return 0; }

    t->id = g_next_id++;
    strncpy(t->name, name ? name : "process", TASK_NAME_MAX - 1);
    t->state = TASK_READY;
    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->switches = 0;
    t->as = as;
    t->exit_code = 0;
    t->as_freed = FALSE;

    /* Раскладка начального стека для process_trampoline (kernel/isr.s):
       сверху вниз -- полный registers_t (то, что POP_ALL_REGS+iretq
       ожидают увидеть под собой), а НИЖЕ него -- точно такой же
       "поддельный" callee-saved-фрейм, что и в task_create(), только
       вместо task_trampoline точкой возврата служит process_trampoline
       и r12/r13 никакой полезной нагрузки не несут (process_trampoline
       их не читает, в отличие от task_trampoline). Благодаря этому
       context_switch() совершенно не отличает эту задачу от обычного
       потока ядра -- секрет целиком в том, КУДА указывает "ret". */
    u64 *sp = (u64 *)(((u64)(stack + TASK_STACK_SIZE)) & ~0xFULL);
    sp -= sizeof(registers_t) / sizeof(u64);
    memcpy(sp, regs, sizeof(registers_t));

    u64 *sp2 = sp;
    *--sp2 = (u64)process_trampoline; /* "адрес возврата" для ret в context_switch */
    *--sp2 = 0;  /* rbp */
    *--sp2 = 0;  /* rbx */
    *--sp2 = 0;  /* r12 */
    *--sp2 = 0;  /* r13 */
    *--sp2 = 0;  /* r14 */
    *--sp2 = 0;  /* r15 */

    t->rsp = (u64)sp2;

    u64 flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);
    t->next = g_current->next;
    g_current->next = t;
    g_task_count++;
    spin_unlock(&g_sched_lock);
    restore_interrupts(flags);

    return t;
}

void task_yield(void) {
    schedule();
}

void task_block_self(void) {
    /* Контракт (см. include/task.h): вызывающий уже отключил прерывания
       и поставил нас в чью-то waitqueue_t ДО этого вызова. Просто
       помечаем состояние и уходим со сцены -- schedule() сама сохранит
       ИСХОДНЫЕ (уже отключённые) флаги прерываний и восстановит ровно
       их же, когда мы в итоге вернёмся сюда же (см. комментарий над
       schedule() и над task_block_self() в task.h). */
    g_current->state = TASK_BLOCKED;
    schedule();
}

void task_wake(task_t *t) {
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_READY;
    }
}

void task_sleep(u32 ms) {
    if (ms == 0) {
        task_yield();
        return;
    }

    u32 freq = timer_frequency();
    if (freq == 0) freq = 100; /* timer_init() ещё не звали -- разумная оценка */

    u64 ticks = ((u64)ms * (u64)freq) / 1000ull;
    if (ticks == 0) ticks = 1; /* хотя бы один тик, иначе ms < 1000/freq "проспится" мгновенно */

    u64 saved_flags = save_and_disable_interrupts();

    g_current->wake_at = timer_ticks() + ticks;
    g_current->is_sleeping = TRUE;
    /* В отличие от task_block_self() из sync.c, здесь никакая
       waitqueue_t не нужна -- будильник (wake_sleepers_if_due выше)
       сам находит нас в кольцевом списке планировщика по is_sleeping,
       поэтому просто помечаем TASK_BLOCKED и уходим тем же путём. */
    g_current->state = TASK_BLOCKED;
    schedule();

    /* Возврат сюда возможен и раньше срока не по нашей вине не бывает:
       wake_sleepers_if_due() -- единственное, что снимает is_sleeping
       и переводит нас в READY, и делает это только когда now >= wake_at.
       is_sleeping к этому моменту уже FALSE. */
    restore_interrupts(saved_flags);
}

/* Вызывается ИЗ task_exit() ПЕРЕД тем, как текущая задача станет
 * TERMINATED: снимает has_waiter и parent со всех прямых детей текущей
 * задачи (т.е. тех, кого она породила через process_fork(), см.
 * process.c), кто бы они ни были -- уже завершившиеся зомби или ещё
 * работающие. Без этого шага ребёнок, чей родитель умер, так и не
 * дождавшись SYS_WAITPID, навсегда остался бы зомби с has_waiter == TRUE:
 * ждать его exit_code стало бы уже некому, а автосборщик
 * (reap_terminated_from()) из уважения к has_waiter никогда бы его не
 * тронул -- классическая утечка. Реального "усыновления процессом 0"
 * тут нет и не нужно: просто разрешаем автосборщику забрать сироту, как
 * если бы у него никогда не было родителя, ожидающего wait() -- то есть
 * ровно то же самое отложенное освобождение (сначала as/ipc, а сам
 * task_t -- только когда он гарантированно уже не prev), что происходит
 * с любой другой задачей без явного ожидающего. Требует отключенных
 * прерываний на всё время обхода кольца: task_t из кольца могут быть
 * освобождены конкурентным reap_terminated_from() (по таймерному тику),
 * и обход "next" уже освобождённого узла -- use-after-free. */
static void orphan_children(task_t *parent) {
    u64 saved_flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    task_t *t = parent->next;
    while (t != parent) {
        task_t *next = t->next;
        if (t->parent == parent) {
            t->parent = 0;
            t->has_waiter = FALSE;
        }
        t = next;
    }

    spin_unlock(&g_sched_lock);
    restore_interrupts(saved_flags);
}

wait_child_result_t task_find_child(task_t *parent, s32 pid, task_t **out_child) {
    u64 saved_flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    wait_child_result_t result = WAIT_CHILD_NONE;
    task_t *t = parent->next;
    while (t != parent) {
        if (t->parent == parent && (pid < 0 || (s32)t->id == pid)) {
            if (t->state == TASK_TERMINATED) {
                if (out_child) *out_child = t;
                result = WAIT_CHILD_DONE;
                break;
            }
            result = WAIT_CHILD_ALIVE; /* есть подходящий, но живой -- продолжаем искать TERMINATED */
        }
        t = t->next;
    }

    spin_unlock(&g_sched_lock);
    restore_interrupts(saved_flags);
    return result;
}

void task_exit(void) {
    orphan_children(g_current);
    g_current->state = TASK_TERMINATED;
    /* Память самой задачи (task_t и её стек) сознательно не
       освобождается здесь: в момент, когда schedule() ниже переключит
       нас на другую задачу, мы физически всё ещё стоим на этом самом
       стеке (context_switch читает/пишет его вплоть до последнего
       ret) -- освобождать его из-под себя небезопасно. Полноценный
       "сборщик мусора" для завершённых задач -- следующий шаг поверх
       этого фундамента (например, отдельный обработчик в schedule(),
       который освобождает TERMINATED-задачи, найденные НЕ текущей). */
    schedule();
    for (;;) { __asm__ volatile ("cli; hlt"); } /* не достигается */
}

/* Обходит кольцо задач и будит тех, кто уснул через task_sleep() и чей
 * срок уже подошёл. Вызывается из scheduler_tick() -- то есть уже из
 * контекста таймерного IRQ (PIT на BSP, локальный LAPIC-таймер на каждом
 * AP, см. include/apic.h), где прерывания и так выключены аппаратно
 * (CPU сбросил IF на входе в interrupt gate). На SMP кольцо -- общая
 * структура (см. g_sched_lock выше): таймерные тики BSP и любого AP
 * могут прийти практически одновременно и обходить кольцо параллельно,
 * поэтому лок обязателен -- в отличие от однопроцессорной версии, здесь
 * уже недостаточно того, что прерывания выключены (это защищает только
 * от повторного входа НА ЭТОМ ЖЕ ядре, но не от другого ядра, делающего
 * то же самое прямо сейчас). O(n) от числа задач, что приемлемо: без
 * этого честный сон в принципе невозможен, а число задач в этой системе
 * заведомо небольшое. */
static void wake_sleepers_if_due(u64 now) {
    spin_lock(&g_sched_lock);

    task_t *start = g_current;
    task_t *t = start;
    do {
        if (t->is_sleeping && now >= t->wake_at) {
            t->is_sleeping = FALSE;
            if (t->state == TASK_BLOCKED) {
                t->state = TASK_READY;
            }
        }
        t = t->next;
    } while (t != start);

    spin_unlock(&g_sched_lock);
}

void scheduler_tick(void) {
    if (g_current == 0) return; /* tasking_init()/task_register_this_cpu() ещё не вызывались на этом ядре */

    wake_sleepers_if_due(timer_ticks());

    /* Счётчик кванта -- per-CPU (this_cpu()->time_slice_ticks, см.
       include/percpu.h): у каждого ядра свой источник тиков (PIT у BSP,
       собственный LAPIC-таймер у каждого AP, см. include/apic.h) и свой
       независимый отсчёт кванта времени -- общий счётчик на все ядра
       не имел бы смысла (то и дело обнулялся бы чужим ядром). */
    percpu_t *cpu = this_cpu();
    cpu->sched_ticks++;
    cpu->time_slice_ticks++;
    if (cpu->time_slice_ticks >= TASK_TIME_SLICE_TICKS) {
        cpu->time_slice_ticks = 0;
        schedule();
    }
}

task_t *task_current(void) {
    return g_current;
}

u32 task_count(void) {
    return g_task_count;
}

task_t *task_get(u32 index) {
    if (g_current == 0 || index >= g_task_count) return 0;

    u64 flags = save_and_disable_interrupts();
    spin_lock(&g_sched_lock);

    task_t *t = g_current;
    for (u32 i = 0; i < index; i++) {
        t = t->next;
    }

    spin_unlock(&g_sched_lock);
    restore_interrupts(flags);
    return t;
}

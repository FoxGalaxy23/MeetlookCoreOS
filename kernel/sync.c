/* sync.c -- см. include/sync.h за общим описанием модели и за общим
 * принципом "спинлок отпускается ДО task_block_self()", который здесь
 * применяется во всех трёх примитивах. */

#include "sync.h"

/* Отдельная от task.c пара cli/sti -- та, что в task.c, объявлена
 * static и намеренно не выставлена наружу (см. её собственный
 * комментарий про то, что снаружи нужно сохранять и восстанавливать
 * именно ИСХОДНЫЙ флаг IF, а не жёстко sti). Здесь та же логика и по
 * той же причине: task_block_self() может вызвать schedule(), которая
 * рассчитывает получить управление обратно с прерываниями в том же
 * состоянии, в котором она их видела на входе. cli защищает от
 * таймерного вытеснения НА ЭТОМ ядре; spinlock_t каждого объекта (см.
 * include/sync.h) защищает от одновременного доступа с ДРУГИХ ядер --
 * нужны оба, ни один не заменяет другой (см. общий комментарий в
 * include/spinlock.h). */
static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(u64 flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* ---- waitqueue_t: простая FIFO из task_t, через поле wq_next ---- */

static void wq_enqueue(waitqueue_t *wq, task_t *t) {
    t->wq_next = 0;
    if (wq->tail) {
        wq->tail->wq_next = t;
    } else {
        wq->head = t;
    }
    wq->tail = t;
}

static task_t *wq_dequeue(waitqueue_t *wq) {
    task_t *t = wq->head;
    if (t) {
        wq->head = t->wq_next;
        if (!wq->head) wq->tail = 0;
        t->wq_next = 0;
    }
    return t;
}

/* ================================ mutex ================================ */

void mutex_init(mutex_t *m) {
    spinlock_init(&m->lock);
    m->locked = FALSE;
    m->owner = 0;
    m->waiters.head = 0;
    m->waiters.tail = 0;
}

void mutex_lock(mutex_t *m) {
    u64 flags = irq_save();
    spin_lock(&m->lock);

    while (m->locked) {
        /* Встаём в очередь и уходим со сцены; ВАЖНО (см. общий
           комментарий в include/sync.h): собственный спинлок объекта
           обязан быть отпущен ДО task_block_self() -- иначе
           mutex_unlock() на другом ядре никогда не смог бы его
           захватить, чтобы нас разбудить (deadlock). После пробуждения
           захватываем спинлок заново и перепроверяем условие в цикле,
           а не считаем, что раз нас разбудили, значит мьютекс уже наш:
           mutex_unlock() лишь ДЕЛАЕТ НАС READY, а не передаёт владение
           напрямую (см. include/sync.h), так что кто-то третий вполне
           мог занять мьютекс раньше, чем планировщик вернулся именно
           к нам -- в т.ч. на другом ядре, параллельно. */
        wq_enqueue(&m->waiters, task_current());
        spin_unlock(&m->lock);
        task_block_self();
        spin_lock(&m->lock);
    }

    m->locked = TRUE;
    m->owner = task_current();

    spin_unlock(&m->lock);
    irq_restore(flags);
}

bool_t mutex_trylock(mutex_t *m) {
    u64 flags = irq_save();
    spin_lock(&m->lock);

    bool_t got_it = FALSE;
    if (!m->locked) {
        m->locked = TRUE;
        m->owner = task_current();
        got_it = TRUE;
    }

    spin_unlock(&m->lock);
    irq_restore(flags);
    return got_it;
}

void mutex_unlock(mutex_t *m) {
    u64 flags = irq_save();
    spin_lock(&m->lock);

    m->locked = FALSE;
    m->owner = 0;

    task_t *t = wq_dequeue(&m->waiters);
    if (t) task_wake(t);

    spin_unlock(&m->lock);
    irq_restore(flags);
}

/* ============================== semaphore =============================== */

void sem_init(semaphore_t *s, s32 initial_count) {
    spinlock_init(&s->lock);
    s->count = initial_count;
    s->waiters.head = 0;
    s->waiters.tail = 0;
}

void sem_wait(semaphore_t *s) {
    u64 flags = irq_save();
    spin_lock(&s->lock);

    while (s->count <= 0) {
        wq_enqueue(&s->waiters, task_current());
        spin_unlock(&s->lock);   /* см. общий комментарий в mutex_lock() выше -- та же причина */
        task_block_self();
        spin_lock(&s->lock);
    }

    s->count--;

    spin_unlock(&s->lock);
    irq_restore(flags);
}

bool_t sem_trywait(semaphore_t *s) {
    u64 flags = irq_save();
    spin_lock(&s->lock);

    bool_t got_it = FALSE;
    if (s->count > 0) {
        s->count--;
        got_it = TRUE;
    }

    spin_unlock(&s->lock);
    irq_restore(flags);
    return got_it;
}

void sem_post(semaphore_t *s) {
    u64 flags = irq_save();
    spin_lock(&s->lock);

    s->count++;

    task_t *t = wq_dequeue(&s->waiters);
    if (t) task_wake(t);

    spin_unlock(&s->lock);
    irq_restore(flags);
}

/* ============================== condvar ================================= */

void cond_init(condvar_t *c) {
    spinlock_init(&c->lock);
    c->waiters.head = 0;
    c->waiters.tail = 0;
}

void cond_wait(condvar_t *c, mutex_t *m) {
    /* Держим ПРЕРЫВАНИЯ выключенными на всём протяжении "встали в
       очередь condvar-а + отпустили мьютекс + уснули" -- иначе между
       отпусканием мьютекса и собственным засыпанием в очередь condvar-а
       на ЭТОМ ЖЕ ядре могло бы влезть чужое cond_signal() и не найти
       нас в очереди (classic lost wakeup). Спинлок самого condvar-а
       (c->lock) защищает от ТОЙ ЖЕ гонки, но исходящей с ДРУГОГО ядра --
       нужны оба (см. общий комментарий в include/sync.h). Порядок
       обязателен: сначала встаём в очередь под c->lock, ПОТОМ
       отпускаем мьютекс m, и только потом отпускаем c->lock и блокируемся
       -- если поменять местами "встали в очередь" и "отпустили мьютекс",
       окно для потерянного пробуждения снова появится. */
    u64 flags = irq_save();
    spin_lock(&c->lock);

    wq_enqueue(&c->waiters, task_current());
    mutex_unlock(m);

    spin_unlock(&c->lock);
    task_block_self();

    irq_restore(flags);

    /* Мьютекс нужно вернуть захваченным перед выходом (см. контракт в
       include/sync.h) -- делаем это уже с восстановленными прерываниями,
       обычным mutex_lock(), который сам умеет блокироваться, если кто-то
       успел перехватить мьютекс раньше нас. */
    mutex_lock(m);
}

void cond_signal(condvar_t *c) {
    u64 flags = irq_save();
    spin_lock(&c->lock);

    task_t *t = wq_dequeue(&c->waiters);
    if (t) task_wake(t);

    spin_unlock(&c->lock);
    irq_restore(flags);
}

void cond_broadcast(condvar_t *c) {
    u64 flags = irq_save();
    spin_lock(&c->lock);

    task_t *t;
    while ((t = wq_dequeue(&c->waiters)) != 0) {
        task_wake(t);
    }

    spin_unlock(&c->lock);
    irq_restore(flags);
}

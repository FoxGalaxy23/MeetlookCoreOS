/* ipc.c -- см. include/ipc.h за общим описанием модели. */

#include "ipc.h"
#include "kheap.h"
#include "string.h"
#include "pmm.h"
#include "paging.h"
#include "memlayout.h"
#include "tcp.h"

/* include/ipc.h дублирует PROC_SHM_SIZE (include/memlayout.h) числом
   0x00200000u явно, чтобы не тянуть memlayout.h в ipc.h -- здесь, где
   memlayout.h всё равно уже включён, проверяем, что дубликат не
   разъехался с оригиналом. */
_Static_assert(SHM_SEGMENT_SIZE * SHM_MAX_SEGMENTS == PROC_SHM_SIZE,
               "SHM_SEGMENT_SIZE (include/ipc.h) разошёлся с PROC_SHM_SIZE (include/memlayout.h)");

/* Отдельная от sync.c/task.c пара cli/sti -- защищает ТОЛЬКО короткие,
 * никогда не блокирующиеся операции над глобальными таблицами g_mqueues/
 * g_shm_segs ниже (поиск свободного/именованного слота, инкремент
 * refcount/attach_count) -- ровно то же самое сочетание "сделать это
 * быстро и атомарно относительно преэмпции таймером, не имея под рукой
 * подходящего mutex_t" уже встречается в kernel/sync.c и kernel/task.c;
 * это тот же самый паттерн, просто предпочтительно не тащить их
 * приватные static-функции через заголовки. Состояние отдельно взятого
 * pipe_t/mqueue_t (в отличие от ЭТИХ таблиц) защищено СВОИМ mutex_t --
 * см. pipe_read()/mq_send() и т.п. ниже, там отдельный irq_save() не
 * нужен. */
static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(u64 flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* =============================== пайпы ================================= */

struct pipe {
    u8 *buf;              /* кольцевой буфер, PIPE_BUF_SIZE байт, из kheap */
    u32 head, tail, count; /* count -- сколько байт сейчас реально в буфере */
    mutex_t lock;
    condvar_t not_empty;   /* сигналится при появлении данных (pipe_write) */
    condvar_t not_full;    /* сигналится при освобождении места (pipe_read) или при readers -> 0 */
    u32 readers;
    u32 writers;
};

bool_t pipe_create(pipe_t **out) {
    if (!out) return FALSE;

    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return FALSE;
    p->buf = (u8 *)kmalloc(PIPE_BUF_SIZE);
    if (!p->buf) { kfree(p); return FALSE; }

    p->head = p->tail = p->count = 0;
    p->readers = 1;
    p->writers = 1;
    mutex_init(&p->lock);
    cond_init(&p->not_empty);
    cond_init(&p->not_full);

    *out = p;
    return TRUE;
}

s32 pipe_read(pipe_t *p, void *buf, u32 len) {
    if (!p || !buf) return -1;
    if (len == 0) return 0;

    mutex_lock(&p->lock);

    while (p->count == 0 && p->writers > 0) {
        cond_wait(&p->not_empty, &p->lock);
    }

    if (p->count == 0) {
        /* писателей больше нет и буфер уже пуст -- EOF, а не ошибка */
        mutex_unlock(&p->lock);
        return 0;
    }

    u32 n = (len < p->count) ? len : p->count;
    for (u32 i = 0; i < n; i++) {
        ((u8 *)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= n;

    cond_signal(&p->not_full);
    mutex_unlock(&p->lock);
    return (s32)n;
}

s32 pipe_write(pipe_t *p, const void *buf, u32 len) {
    if (!p || !buf) return -1;
    if (len == 0) return 0;

    mutex_lock(&p->lock);

    while (p->count == PIPE_BUF_SIZE && p->readers > 0) {
        cond_wait(&p->not_full, &p->lock);
    }

    if (p->readers == 0) {
        /* некому читать -- аналог EPIPE, не блокируем дальше */
        mutex_unlock(&p->lock);
        return -1;
    }

    u32 free_space = PIPE_BUF_SIZE - p->count;
    u32 n = (len < free_space) ? len : free_space;
    for (u32 i = 0; i < n; i++) {
        p->buf[p->head] = ((const u8 *)buf)[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += n;

    cond_signal(&p->not_empty);
    mutex_unlock(&p->lock);
    return (s32)n;
}

void pipe_addref_read(pipe_t *p) {
    if (!p) return;
    mutex_lock(&p->lock);
    p->readers++;
    mutex_unlock(&p->lock);
}

void pipe_addref_write(pipe_t *p) {
    if (!p) return;
    mutex_lock(&p->lock);
    p->writers++;
    mutex_unlock(&p->lock);
}

void pipe_close_read(pipe_t *p) {
    if (!p) return;

    mutex_lock(&p->lock);
    if (p->readers > 0) p->readers--;
    if (p->readers == 0) {
        /* писатели, ждущие места (cond_wait(&p->not_full,...)) должны
           проснуться и увидеть readers == 0, а не зависнуть навсегда */
        cond_broadcast(&p->not_full);
    }
    bool_t should_free = (p->readers == 0 && p->writers == 0);
    mutex_unlock(&p->lock);

    if (should_free) {
        kfree(p->buf);
        kfree(p);
    }
}

void pipe_close_write(pipe_t *p) {
    if (!p) return;

    mutex_lock(&p->lock);
    if (p->writers > 0) p->writers--;
    if (p->writers == 0) {
        /* читатели, ждущие данных, должны проснуться и увидеть
           writers == 0 -> вернуть EOF, а не зависнуть навсегда */
        cond_broadcast(&p->not_empty);
    }
    bool_t should_free = (p->readers == 0 && p->writers == 0);
    mutex_unlock(&p->lock);

    if (should_free) {
        kfree(p->buf);
        kfree(p);
    }
}

/* ============================ очереди сообщений ========================= */

typedef struct mq_msg {
    u32 len;
    u8 data[MQ_MAX_MSG_SIZE];
} mq_msg_t;

struct mqueue {
    char name[MQ_NAME_MAX];   /* пусто ("") -- анонимная */
    bool_t in_use;
    mq_msg_t msgs[MQ_MAX_MSGS];
    u32 head, tail, count;
    mutex_t lock;
    condvar_t not_empty;
    condvar_t not_full;
    u32 refcount;
};

static mqueue_t g_mqueues[MQ_MAX_QUEUES];

mqueue_t *mq_open_or_create(const char *name) {
    u64 flags = irq_save();

    if (name) {
        for (u32 i = 0; i < MQ_MAX_QUEUES; i++) {
            if (g_mqueues[i].in_use && strncmp(g_mqueues[i].name, name, MQ_NAME_MAX) == 0) {
                g_mqueues[i].refcount++;
                irq_restore(flags);
                return &g_mqueues[i];
            }
        }
    }

    for (u32 i = 0; i < MQ_MAX_QUEUES; i++) {
        if (!g_mqueues[i].in_use) {
            mqueue_t *q = &g_mqueues[i];
            memset(q, 0, sizeof(*q));
            q->in_use = TRUE;
            q->refcount = 1;
            mutex_init(&q->lock);
            cond_init(&q->not_empty);
            cond_init(&q->not_full);
            if (name) strncpy(q->name, name, MQ_NAME_MAX - 1);
            irq_restore(flags);
            return q;
        }
    }

    irq_restore(flags);
    return 0; /* все MQ_MAX_QUEUES слотов заняты */
}

s32 mq_send(mqueue_t *q, const void *data, u32 len) {
    if (!q || !data || len == 0 || len > MQ_MAX_MSG_SIZE) return -1;

    mutex_lock(&q->lock);
    while (q->count == MQ_MAX_MSGS) {
        cond_wait(&q->not_full, &q->lock);
    }

    mq_msg_t *m = &q->msgs[q->head];
    memcpy(m->data, data, len);
    m->len = len;
    q->head = (q->head + 1) % MQ_MAX_MSGS;
    q->count++;

    cond_signal(&q->not_empty);
    mutex_unlock(&q->lock);
    return 0;
}

s32 mq_recv(mqueue_t *q, void *buf, u32 maxlen) {
    if (!q || !buf) return -1;

    mutex_lock(&q->lock);
    while (q->count == 0) {
        cond_wait(&q->not_empty, &q->lock);
    }

    mq_msg_t *m = &q->msgs[q->tail];
    if (m->len > maxlen) {
        /* буфер мал -- сообщение остаётся в очереди на будущее */
        mutex_unlock(&q->lock);
        return -1;
    }

    memcpy(buf, m->data, m->len);
    s32 n = (s32)m->len;
    q->tail = (q->tail + 1) % MQ_MAX_MSGS;
    q->count--;

    cond_signal(&q->not_full);
    mutex_unlock(&q->lock);
    return n;
}

void mq_addref(mqueue_t *q) {
    if (!q) return;
    u64 flags = irq_save();
    q->refcount++;
    irq_restore(flags);
}

void mq_close(mqueue_t *q) {
    if (!q) return;
    u64 flags = irq_save();
    if (q->refcount > 0) q->refcount--;
    if (q->refcount == 0) q->in_use = FALSE; /* слот свободен для будущего mq_open_or_create() */
    irq_restore(flags);
}

/* =========================== разделяемая память ========================== */

struct shm_seg {
    char name[SHM_NAME_MAX];
    bool_t in_use;
    bool_t marked_destroy;
    u32 size;                       /* запрошенный размер, байт */
    u32 num_pages;
    void *frames[SHM_SEGMENT_PAGES]; /* физические адреса кадров (не обязательно подряд идущих) */
    u32 attach_count;
};

static shm_seg_t g_shm_segs[SHM_MAX_SEGMENTS];

s32 shm_open_or_create(const char *name, u32 size) {
    if (size == 0 || size > SHM_SEGMENT_SIZE) return -1;

    u64 flags = irq_save();

    if (name) {
        for (u32 i = 0; i < SHM_MAX_SEGMENTS; i++) {
            if (g_shm_segs[i].in_use && !g_shm_segs[i].marked_destroy &&
                strncmp(g_shm_segs[i].name, name, SHM_NAME_MAX) == 0) {
                irq_restore(flags);
                return (s32)i; /* существующий именованный сегмент, size игнорируется */
            }
        }
    }

    for (u32 i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (g_shm_segs[i].in_use) continue;

        /* Застолбили слот и сразу отпускаем cli: выделение физических
           кадров (pmm_alloc_frame()) ниже не блокирует и не спит, но
           было бы неаккуратно держать прерывания выключенными на время
           цикла из нескольких memset()'ов по 4 KiB -- это не тот
           короткий O(1) случай, ради которого вообще существует
           irq_save() в этом файле. Слот уже помечен in_use, поэтому
           никто другой его не перехватит, даже если нас прямо сейчас
           прервёт таймер. */
        g_shm_segs[i].in_use = TRUE;
        irq_restore(flags);

        shm_seg_t *seg = &g_shm_segs[i];
        memset(seg->name, 0, SHM_NAME_MAX);
        if (name) strncpy(seg->name, name, SHM_NAME_MAX - 1);
        seg->marked_destroy = FALSE;
        seg->attach_count = 0;
        seg->size = size;
        seg->num_pages = (size + 4095u) / 4096u;

        for (u32 p = 0; p < seg->num_pages; p++) {
            void *frame = pmm_alloc_frame();
            if (!frame) {
                /* нехватка физической памяти -- откатываем уже
                   выделенные кадры и освобождаем слот */
                for (u32 j = 0; j < p; j++) pmm_frame_release(seg->frames[j]);
                u64 f2 = irq_save();
                seg->in_use = FALSE;
                irq_restore(f2);
                return -2;
            }
            memset(frame, 0, 4096); /* физическая память identity-mapped, см. kernel/paging.c */
            seg->frames[p] = frame;
        }

        return (s32)i;
    }

    irq_restore(flags);
    return -1; /* все SHM_MAX_SEGMENTS слотов заняты */
}

/* См. include/ipc.h за общей идеей. Сознательно НЕ переиспользует тело
   shm_open_or_create() выше (можно было бы выделить общий хелпер
   "найти слот по имени/завести новый", но эти две функции расходятся
   ровно в том единственном месте, где легко ошибиться -- ОТКУДА берутся
   frames[] -- так что разделение целиком, при небольшом дублировании
   цикла поиска слота, безопаснее, чем один хелпер с веткой "выделять
   самому или взять готовое"). */
s32 shm_create_from_frames(const char *name, void **frames, u32 num_pages, u32 size) {
    if (!name || !frames || num_pages == 0 || num_pages > SHM_SEGMENT_PAGES) return -1;
    if (size == 0 || size > SHM_SEGMENT_SIZE) return -1;

    u64 flags = irq_save();

    for (u32 i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (g_shm_segs[i].in_use && !g_shm_segs[i].marked_destroy &&
            strncmp(g_shm_segs[i].name, name, SHM_NAME_MAX) == 0) {
            irq_restore(flags);
            return (s32)i; /* уже зарегистрирован -- см. общий комментарий в include/ipc.h */
        }
    }

    for (u32 i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (g_shm_segs[i].in_use) continue;

        g_shm_segs[i].in_use = TRUE;
        irq_restore(flags);

        shm_seg_t *seg = &g_shm_segs[i];
        memset(seg->name, 0, SHM_NAME_MAX);
        strncpy(seg->name, name, SHM_NAME_MAX - 1);
        seg->marked_destroy = FALSE;
        seg->attach_count = 0;
        seg->size = size;
        seg->num_pages = num_pages;
        for (u32 p = 0; p < num_pages; p++) seg->frames[p] = frames[p];

        return (s32)i;
    }

    irq_restore(flags);
    return -1; /* все SHM_MAX_SEGMENTS слотов заняты */
}

shm_seg_t *shm_get(s32 id) {
    if (id < 0 || (u32)id >= SHM_MAX_SEGMENTS) return 0;
    if (!g_shm_segs[id].in_use) return 0;
    return &g_shm_segs[id];
}

bool_t shm_attach(shm_seg_t *seg, task_t *t, u64 *out_vaddr) {
    if (!seg || !t || !t->as || !out_vaddr) return FALSE;

    u32 id = (u32)(seg - g_shm_segs);
    u64 vaddr = PROC_SHM_BASE + (u64)id * SHM_SEGMENT_SIZE;

    /* Повторное присоединение того же сегмента этой же задачей не
       поддерживается (в реальном SysV shm это тоже, как правило, не
       имеет смысла -- один и тот же процесс просто получил бы второй
       адрес того же самого сегмента). */
    for (u32 i = 0; i < TASK_MAX_IPC_HANDLES; i++) {
        if (t->ipc_handles[i].type == IPC_HANDLE_SHM && t->ipc_handles[i].vaddr == vaddr) {
            return FALSE;
        }
    }

    for (u32 p = 0; p < seg->num_pages; p++) {
        pmm_frame_addref(seg->frames[p]);
        /* executable=FALSE -- см. W^X-комментарий в include/paging.h и
           kernel/paging.c:as_clone_cow(): разделяемая память никогда не
           должна быть исполняемой, независимо от writable. */
        as_map_frame(t->as, vaddr + (u64)p * 4096u, (u64)(uintptr_t)seg->frames[p], TRUE, FALSE, FALSE);
    }

    s32 h = ipc_handle_alloc(t, IPC_HANDLE_SHM, seg, vaddr);
    if (h < 0) {
        /* таблица хендлов процесса переполнена -- откатываем отображение */
        as_unmap_range(t->as, vaddr, seg->num_pages * 4096u);
        return FALSE;
    }

    u64 flags = irq_save();
    seg->attach_count++;
    irq_restore(flags);

    *out_vaddr = vaddr;
    return TRUE;
}

bool_t shm_detach_by_vaddr(task_t *t, u64 vaddr) {
    if (!t) return FALSE;

    s32 h = -1;
    for (u32 i = 0; i < TASK_MAX_IPC_HANDLES; i++) {
        if (t->ipc_handles[i].type == IPC_HANDLE_SHM && t->ipc_handles[i].vaddr == vaddr) {
            h = (s32)i;
            break;
        }
    }
    if (h < 0) return FALSE;

    shm_seg_t *seg = (shm_seg_t *)t->ipc_handles[h].obj;

    as_unmap_range(t->as, vaddr, seg->num_pages * 4096u);

    t->ipc_handles[h].type = IPC_HANDLE_FREE;
    t->ipc_handles[h].obj = 0;
    t->ipc_handles[h].vaddr = 0;

    u64 flags = irq_save();
    if (seg->attach_count > 0) seg->attach_count--;
    bool_t should_free = (seg->attach_count == 0 && seg->marked_destroy);
    if (should_free) seg->in_use = FALSE;
    irq_restore(flags);

    if (should_free) {
        for (u32 p = 0; p < seg->num_pages; p++) pmm_frame_release(seg->frames[p]);
    }

    return TRUE;
}

bool_t shm_mark_destroy(s32 id) {
    shm_seg_t *seg = shm_get(id);
    if (!seg) return FALSE;

    u64 flags = irq_save();
    seg->marked_destroy = TRUE;
    bool_t should_free = (seg->attach_count == 0);
    if (should_free) seg->in_use = FALSE;
    irq_restore(flags);

    if (should_free) {
        for (u32 p = 0; p < seg->num_pages; p++) pmm_frame_release(seg->frames[p]);
    }
    return TRUE;
}

/* ============ интеграция с task_t: fork() и завершение задачи ============ */

s32 ipc_handle_alloc(task_t *t, u32 type, void *obj, u64 vaddr) {
    if (!t) return -1;
    for (u32 i = 0; i < TASK_MAX_IPC_HANDLES; i++) {
        if (t->ipc_handles[i].type == IPC_HANDLE_FREE) {
            t->ipc_handles[i].type = type;
            t->ipc_handles[i].obj = obj;
            t->ipc_handles[i].vaddr = vaddr;
            return (s32)i;
        }
    }
    return -1; /* TASK_MAX_IPC_HANDLES исчерпаны */
}

void *ipc_handle_get(task_t *t, s32 h, u32 expected_type) {
    if (!t || h < 0 || (u32)h >= TASK_MAX_IPC_HANDLES) return 0;
    if (t->ipc_handles[h].type != expected_type) return 0;
    return t->ipc_handles[h].obj;
}

static void handle_free_slot(task_t *t, s32 h) {
    t->ipc_handles[h].type = IPC_HANDLE_FREE;
    t->ipc_handles[h].obj = 0;
    t->ipc_handles[h].vaddr = 0;
}

bool_t ipc_handle_close(task_t *t, s32 h) {
    if (!t || h < 0 || (u32)h >= TASK_MAX_IPC_HANDLES) return FALSE;

    u32 type = t->ipc_handles[h].type;
    void *obj = t->ipc_handles[h].obj;
    if (type == IPC_HANDLE_FREE) return FALSE;

    switch (type) {
        case IPC_HANDLE_PIPE_READ:  pipe_close_read((pipe_t *)obj);   break;
        case IPC_HANDLE_PIPE_WRITE: pipe_close_write((pipe_t *)obj);  break;
        case IPC_HANDLE_MQUEUE:     mq_close((mqueue_t *)obj);        break;
        case IPC_HANDLE_TCP_SOCKET: tcp_close((s32)(intptr_t)obj - 1); break;
        default:
            /* IPC_HANDLE_SHM сюда попадать не должен -- у shm свой явный
               путь закрытия по адресу, см. shm_detach_by_vaddr(). */
            return FALSE;
    }

    handle_free_slot(t, h);
    return TRUE;
}

void ipc_task_fork_copy(task_t *parent, task_t *child) {
    if (!parent || !child) return;

    for (u32 i = 0; i < TASK_MAX_IPC_HANDLES; i++) {
        u32 type = parent->ipc_handles[i].type;
        if (type == IPC_HANDLE_FREE) continue;

        void *obj = parent->ipc_handles[i].obj;
        child->ipc_handles[i].type = type;
        child->ipc_handles[i].obj = obj;
        child->ipc_handles[i].vaddr = parent->ipc_handles[i].vaddr;

        switch (type) {
            case IPC_HANDLE_PIPE_READ:
                pipe_addref_read((pipe_t *)obj);
                break;
            case IPC_HANDLE_PIPE_WRITE:
                pipe_addref_write((pipe_t *)obj);
                break;
            case IPC_HANDLE_MQUEUE:
                mq_addref((mqueue_t *)obj);
                break;
            case IPC_HANDLE_SHM: {
                /* Физическое разделение страниц между parent и child уже
                   обеспечено as_clone_cow() (kernel/paging.c, особый
                   случай для shm-слота) -- здесь только синхронизируем
                   attach_count сегмента с этим фактом. */
                shm_seg_t *seg = (shm_seg_t *)obj;
                u64 flags = irq_save();
                seg->attach_count++;
                irq_restore(flags);
                break;
            }
            default:
                break;
        }
    }
}

void ipc_task_cleanup(task_t *t) {
    if (!t) return;

    for (u32 i = 0; i < TASK_MAX_IPC_HANDLES; i++) {
        u32 type = t->ipc_handles[i].type;
        if (type == IPC_HANDLE_FREE) continue;

        void *obj = t->ipc_handles[i].obj;

        switch (type) {
            case IPC_HANDLE_PIPE_READ:
                pipe_close_read((pipe_t *)obj);
                break;
            case IPC_HANDLE_PIPE_WRITE:
                pipe_close_write((pipe_t *)obj);
                break;
            case IPC_HANDLE_MQUEUE:
                mq_close((mqueue_t *)obj);
                break;
            case IPC_HANDLE_TCP_SOCKET:
                tcp_close((s32)(intptr_t)obj - 1);
                break;
            case IPC_HANDLE_SHM: {
                /* НЕ трогаем PTE/физические кадры этого отображения --
                   их освободит общий проход as_destroy() по процессному
                   окну (см. kernel/task.c, вызывается рядом с этой
                   функцией) точно так же, как и для COW-кадров. Здесь
                   только бухгалтерия самого сегмента. */
                shm_seg_t *seg = (shm_seg_t *)obj;
                u64 flags = irq_save();
                bool_t should_free = FALSE;
                if (seg->attach_count > 0) seg->attach_count--;
                if (seg->attach_count == 0 && seg->marked_destroy) {
                    should_free = TRUE;
                    seg->in_use = FALSE;
                }
                irq_restore(flags);
                if (should_free) {
                    for (u32 p = 0; p < seg->num_pages; p++) pmm_frame_release(seg->frames[p]);
                }
                break;
            }
            default:
                break;
        }

        handle_free_slot(t, (s32)i);
    }
}

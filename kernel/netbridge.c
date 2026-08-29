/* netbridge.c -- см. include/netbridge.h за общей идеей. */

#include "netbridge.h"
#include "net.h"
#include "eth.h"
#include "sync.h"
#include "string.h"
#include "kprintf.h"

typedef struct {
    u32 len;
    u8 data[NETBRIDGE_MAX_FRAME];
} netframe_t;

/* Кольцевой буфер кадров + семафор, считающий, сколько их сейчас готово
   к выемке -- ровно тот же паттерн производитель/потребитель, что и у
   g_irq_sem[] в kernel/irqbridge.c, только с полезной нагрузкой (кадр),
   а не голым событием. head/tail/count защищены net_irq_save()/
   net_irq_restore() (см. общий комментарий в include/netbridge.h про
   то, почему НЕ mutex_t), а не отдельным spinlock_t -- тот же уровень
   строгости, что уже применяет net/net.c и drivers/rtl8139.c ко всем
   своим собственным разделяемым структурам (ARP-кэш, TX-дескрипторы);
   заводить более сильную защиту только для этого моста, пока остальной
   сетевой стек рядом полагается на cli/sti, было бы непоследовательно
   и не спасло бы от гонок в других местах того же стека. */
typedef struct {
    netframe_t frames[NETBRIDGE_QUEUE_SLOTS];
    u32 head, tail, count;
    semaphore_t items;
} netframe_queue_t;

static netframe_queue_t g_tx_queue; /* ядро -> драйвер-контейнер */
static netframe_queue_t g_rx_queue; /* драйвер-контейнер -> ядро */

static bool_t frameq_try_push(netframe_queue_t *q, const void *data, u32 len) {
    if (!data || len == 0 || len > NETBRIDGE_MAX_FRAME) return FALSE;

    u64 flags = net_irq_save();
    if (q->count == NETBRIDGE_QUEUE_SLOTS) {
        net_irq_restore(flags);
        return FALSE; /* очередь полна -- кадр отбрасывается, см. общий комментарий в netbridge.h */
    }

    netframe_t *f = &q->frames[q->head];
    memcpy(f->data, data, len);
    f->len = len;
    q->head = (q->head + 1) % NETBRIDGE_QUEUE_SLOTS;
    q->count++;
    net_irq_restore(flags);

    /* sem_post() сам оборачивает свою критическую секцию в irq_save()/
       irq_restore() (kernel/sync.c) -- безопасен здесь же, даже если
       frameq_try_push() сама вызвана из настоящего обработчика
       прерывания (TX-очередь, будущий eth_send(), Фаза 4.4). */
    sem_post(&q->items);
    return TRUE;
}

/* Блокирует вызывающего (sem_wait -- НЕЛЬЗЯ звать из обработчика
   прерывания, только из обычного task-контекста: ring-3 syscall или
   поток ядра, см. include/sync.h), пока не появится кадр, потом
   копирует его в buf. Если maxlen меньше длины кадра -- кадр всё равно
   изымается из очереди (в отличие от mq_recv(), см. NETBRIDGE_H за
   объяснением, почему тут так безопаснее) и функция возвращает <0. */
static s32 frameq_pop_blocking(netframe_queue_t *q, void *buf, u32 maxlen) {
    sem_wait(&q->items);

    u64 flags = net_irq_save();
    netframe_t *f = &q->frames[q->tail];
    u32 len = f->len;
    s32 ret;
    if (len > maxlen) {
        ret = -1;
    } else {
        memcpy(buf, f->data, len);
        ret = (s32)len;
    }
    q->tail = (q->tail + 1) % NETBRIDGE_QUEUE_SLOTS;
    q->count--;
    net_irq_restore(flags);
    return ret;
}

void netbridge_init(void) {
    memset(&g_tx_queue, 0, sizeof(g_tx_queue));
    memset(&g_rx_queue, 0, sizeof(g_rx_queue));
    sem_init(&g_tx_queue.items, 0);
    sem_init(&g_rx_queue.items, 0);
}

bool_t netbridge_push_tx_frame(const void *frame, u32 len) {
    return frameq_try_push(&g_tx_queue, frame, len);
}

void netbridge_rx_pump(void *arg) {
    (void)arg;
    u8 buf[NETBRIDGE_MAX_FRAME];

    for (;;) {
        s32 len = frameq_pop_blocking(&g_rx_queue, buf, sizeof(buf));
        if (len < 0) {
            /* Не должно происходить: buf здесь ровно NETBRIDGE_MAX_FRAME,
               то есть максимум, который вообще мог оказаться в очереди
               (frameq_try_push() сама это гарантирует), но проверяем
               честно, а не полагаемся молча. */
            continue;
        }

        /* Временная диагностика этого подшага (4.3): пока ни один
           настоящий драйвер ещё не подключён к каналу (см. Фаза 4.4 в
           include/netbridge.h), единственный способ увидеть, что кадр
           реально дошёл от контейнера до ядра -- напечатать это здесь.
           Когда rtl8139d (Фаза 4.4) станет единственным производителем
           RX-очереди, эта строка станет слишком частой (кадр на каждый
           реальный пакет) и её стоит будет убрать или спрятать за
           отдельный debug-флаг -- см. --debug в mlbrowser за похожим
           прецедентом в другом проекте. */
        kprintf("netbridge: rx pump got %d byte(s) from driver container, handing to eth_input()\n", len);
        eth_input(buf, (u32)len);
    }
}

s32 netbridge_sys_tx_dequeue(void *buf, u32 maxlen) {
    return frameq_pop_blocking(&g_tx_queue, buf, maxlen);
}

s32 netbridge_sys_rx_enqueue(const void *buf, u32 len) {
    return frameq_try_push(&g_rx_queue, buf, len) ? 0 : -1;
}

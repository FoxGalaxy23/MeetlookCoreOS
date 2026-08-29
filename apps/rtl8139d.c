/* rtl8139d.c -- сетевой драйвер как обычный ring-3 контейнер (Фаза 4,
 * подшаг 4, см. CONTAINER_ARCHITECTURE.md и include/netdrv.h). То, что
 * раньше было drivers/rtl8139.c (весь протокол общения с картой после
 * первоначального обнаружения) -- теперь здесь.
 *
 * Запускается АВТОМАТИЧЕСКИ при старте ядра, если найдена карта (см.
 * kernel/netdrv.c: netdrv_launch()) -- не вручную из шелла. io_base и
 * irq_line передаются через argv[1]/argv[2] (десятичные строки), а не
 * захардкожены и не прочитаны из манифеста -- см. общий комментарий "Как
 * контейнер узнаёт io_base/irq_line" в include/netdrv.h.
 *
 * Физические DMA-буферы кольца приёма и буферов передачи выделены и уже
 * прописаны в регистры RBSTART/TSAD0..3 самим ядром (kernel/netdrv.c) ДО
 * запуска этого контейнера -- см. тот же общий комментарий за тем, почему
 * обычный ring-3 процесс в принципе не может знать физический адрес
 * собственной памяти. Здесь их содержимое доступно через SYS_SHM_CREATE/
 * SYS_SHM_ATTACH по именам "netdrv-rx"/"netdrv-tx" -- тот же CAP_SHM
 * примитив, которым уже пользуется apps/ipctest.c.
 *
 * ---- Почему два процесса (fork()), а не один ----
 *
 * У этой ОС нет потоков внутри одного адресного пространства для ring-3
 * кода (только процессы, см. include/process.h) и нет select()/poll() --
 * а драйверу нужно ОДНОВРЕМЕННО и блокирующе ждать IRQ (sys_irq_wait()),
 * и блокирующе ждать кадры на отправку (sys_netbridge_tx_dequeue()) --
 * два РАЗНЫХ блокирующих ожидания, которые в общем случае могут
 * произойти в любом порядке. Решение -- ровно то же, что и в
 * apps/ipctest.c: sys_shm_attach() ОБЕИХ shm-сегментов ДО fork(), чтобы
 * потомок унаследовал те же самые отображения (сегмент остаётся
 * ДЕЙСТВИТЕЛЬНО общим, а не COW-копией при первой записи -- см.
 * as_clone_cow(), kernel/paging.c, и комментарий в apps/ipctest.c), затем
 * разделение на две роли: потомок ждёт IRQ и разбирает кольцо приёма,
 * родитель ждёт кадры из TX-очереди и кладёт их в буфер передачи.
 * IMR/RCR/TCR/CMD программируются ОДИН раз, ДО fork() -- это общие для
 * карты регистры, а не что-то, за что отвечает конкретно одна из ролей.
 */

#include "mlapp.h"
#include "libc.h"
#include "io.h"

/* ---- смещения регистров относительно I/O base (BAR0) -- см. общий
   комментарий в kernel/netdrv.c за тем, какая часть регистров уже
   запрограммирована там, а какая -- здесь. ---- */
#define REG_TSD0    0x10
#define REG_CMD     0x37
#define REG_CAPR    0x38
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44

#define CMD_RE   0x08
#define CMD_TE   0x04
#define CMD_BUFE 0x01

#define ISR_ROK   0x0001
#define ISR_RER   0x0002
#define ISR_TOK   0x0004
#define ISR_TER   0x0008
#define ISR_RXOVW 0x0010

#define RCR_APM  (1u << 1)
#define RCR_AM   (1u << 2)
#define RCR_AB   (1u << 3)
#define RCR_WRAP (1u << 7)

#define TSD_TOK  (1u << 15)
#define TSD_TABT (1u << 30)

#define RTL_MAX_FRAME 1514u /* == NETBRIDGE_MAX_FRAME, include/netbridge.h */

/* ДОЛЖНО совпадать с одноимёнными константами в kernel/netdrv.c -- тот
   же физический буфер, оба конца обязаны одинаково понимать его
   границы (см. общий комментарий там же). */
#define RX_BUF_LEN (8192u + 16u + 1500u)
#define TX_BUF_COUNT 4u
#define TX_SLOT_STRIDE 4096u

/* ---- роль "приём": ждёт IRQ, разбирает кольцо, шлёт кадры в ядро ---- */
static void rx_role(u16 io_base, u32 irq_line, u8 *rx_base) {
    u32 rx_offset = 0;
    printf("rtl8139d: rx role ready, waiting on irq %u\n", irq_line);

    for (;;) {
        s32 rc = sys_irq_wait(irq_line);
        if (rc < 0) {
            printf("rtl8139d: sys_irq_wait failed, rc=%d -- rx role exiting\n", rc);
            sys_exit(1);
        }

        u16 status = inw(io_base + REG_ISR);
        if (status == 0) continue;
        /* Подтверждаем ВСЕ выставленные биты сразу -- в т.ч. TOK/TER
           (иначе карта перестанет генерировать новые IRQ для передачи,
           хотя саму передачу здесь не обслуживаем -- см. tx_role() ниже,
           который следит за TSD напрямую, не через ISR/IRQ). */
        outw(io_base + REG_ISR, status);

        if (!(status & (ISR_ROK | ISR_RXOVW))) continue;

        /* Может прийти сразу несколько кадров за одно прерывание --
           вычитываем ВСЕ, пока RX-буфер не окажется пуст (бит BUFE). */
        while (!(inb(io_base + REG_CMD) & CMD_BUFE)) {
            u8 *hdr = rx_base + rx_offset;
            u16 pkt_status = (u16)hdr[0] | ((u16)hdr[1] << 8);
            u16 pkt_len    = (u16)hdr[2] | ((u16)hdr[3] << 8); /* включая 4-байтный CRC */

            if (!(pkt_status & 0x0001) || pkt_len < 8 || pkt_len > (RTL_MAX_FRAME + 4)) {
                break; /* признак рассинхронизации указателя -- см. комментарий в старом drivers/rtl8139.c */
            }

            u32 data_off = rx_offset + 4;
            u32 frame_len = (u32)pkt_len - 4;

            if (data_off + frame_len <= RX_BUF_LEN) {
                sys_netbridge_rx_enqueue(rx_base + data_off, frame_len);
            } else {
                /* Кадр физически перенесён через границу кольца --
                   собираем во временный линейный буфер (см. тот же
                   приём в старом drivers/rtl8139.c). */
                u8 tmp[RTL_MAX_FRAME];
                u32 first = RX_BUF_LEN - data_off;
                if (first > frame_len) first = frame_len;
                memcpy(tmp, rx_base + data_off, first);
                if (frame_len > first) memcpy(tmp + first, rx_base, frame_len - first);
                sys_netbridge_rx_enqueue(tmp, frame_len);
            }

            rx_offset = (rx_offset + pkt_len + 4 + 3) & ~3u;
            if (rx_offset >= 8192u) rx_offset -= 8192u;
            outw(io_base + REG_CAPR, (u16)(rx_offset - 16));
        }
    }
}

/* ---- роль "передача": ждёт кадры из netbridge, кладёт их в TX-слот ---- */
static void tx_role(u16 io_base, u8 *tx_base) {
    u32 cur_tx = 0;
    printf("rtl8139d: tx role ready, waiting on netbridge TX queue\n");

    u8 buf[RTL_MAX_FRAME];
    for (;;) {
        s32 len = sys_netbridge_tx_dequeue(buf, sizeof(buf));
        if (len < 0) {
            printf("rtl8139d: sys_netbridge_tx_dequeue failed, rc=%d -- tx role exiting\n", len);
            sys_exit(1);
        }

        u32 slot = cur_tx % TX_BUF_COUNT;
        cur_tx++;
        u8 *slot_buf = tx_base + slot * TX_SLOT_STRIDE;

        u32 padded_len = (u32)len < 60u ? 60u : (u32)len; /* минимальный размер кадра Ethernet без CRC */
        memcpy(slot_buf, buf, (u32)len);
        if (padded_len > (u32)len) memset(slot_buf + len, 0, padded_len - (u32)len);

        /* TSAD этого слота уже прописан ядром (kernel/netdrv.c) и не
           меняется между вызовами -- запись в TSD запускает передачу;
           младшие 13 бит -- длина. */
        outl(io_base + REG_TSD0 + slot * 4, padded_len);

        /* Ждём подтверждения (TOK) либо ошибки -- ограниченный busy-poll,
           передача на 100 Мбит занимает микросекунды (тот же приём, что
           и в старом drivers/rtl8139.c). */
        for (u32 i = 0; i < 2000000u; i++) {
            u32 tsd = inl(io_base + REG_TSD0 + slot * 4);
            if (tsd & (TSD_TOK | TSD_TABT)) break;
        }
    }
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    if (argc < 3) {
        printf("rtl8139d: usage: rtl8139d <io_base> <irq>\n");
        printf("rtl8139d: normally launched automatically at boot by kernel/netdrv.c,\n");
        printf("rtl8139d: not by hand -- see rootfs/containers/rtl8139d.manifest for\n");
        printf("rtl8139d: manual debugging after a crash.\n");
        sys_exit(1);
    }

    u16 io_base = (u16)atoi(argv[1]);
    u32 irq_line = (u32)atoi(argv[2]);

    printf("rtl8139d: starting, io_base=0x%x irq=%u\n", (u32)io_base, irq_line);

    s32 rx_id = sys_shm_create("netdrv-rx", RX_BUF_LEN);
    if (rx_id < 0) {
        printf("rtl8139d: sys_shm_create(netdrv-rx) failed, rc=%d\n", rx_id);
        sys_exit(1);
    }
    void *rx_vaddr = 0;
    if (sys_shm_attach(rx_id, &rx_vaddr) < 0) {
        printf("rtl8139d: sys_shm_attach(netdrv-rx) failed\n");
        sys_exit(1);
    }

    s32 tx_id = sys_shm_create("netdrv-tx", TX_BUF_COUNT * TX_SLOT_STRIDE);
    if (tx_id < 0) {
        printf("rtl8139d: sys_shm_create(netdrv-tx) failed, rc=%d\n", tx_id);
        sys_exit(1);
    }
    void *tx_vaddr = 0;
    if (sys_shm_attach(tx_id, &tx_vaddr) < 0) {
        printf("rtl8139d: sys_shm_attach(netdrv-tx) failed\n");
        sys_exit(1);
    }

    /* ---- включаем карту -- RBSTART/TSAD* уже настроены ядром
       (kernel/netdrv.c), см. общий комментарий вверху файла и в
       include/netdrv.h. ВАЖНО: должно произойти ДО fork() ниже -- после
       него обе роли (rx/tx) параллельны, а IMR/RCR/TCR/CMD -- общие для
       карты регистры, программировать их дважды не нужно и незачем
       координировать, кто из двух это сделает. ---- */
    outw(io_base + REG_IMR, ISR_ROK | ISR_RER | ISR_TOK | ISR_TER | ISR_RXOVW);
    outl(io_base + REG_RCR,
         RCR_APM | RCR_AM | RCR_AB | RCR_WRAP |
         (0x7u << 8)  /* MXDMA = unlimited */ |
         (0x7u << 13) /* RXFTH = no threshold */);
    outl(io_base + REG_TCR, (0x7u << 8));
    outb(io_base + REG_CMD, CMD_RE | CMD_TE);

    printf("rtl8139d: card enabled (RE|TE), splitting into rx/tx roles via fork()\n");

    s32 pid = sys_fork();
    if (pid < 0) {
        printf("rtl8139d: fork() failed -- driver cannot run (needs separate rx/tx roles,\n");
        printf("rtl8139d: see the comment at the top of this file)\n");
        sys_exit(1);
    }

    if (pid == 0) {
        rx_role(io_base, irq_line, (u8 *)rx_vaddr);
    } else {
        tx_role(io_base, (u8 *)tx_vaddr);
    }

    sys_exit(0); /* не достигается -- обе роли крутятся в бесконечном цикле */
    return 0;
}

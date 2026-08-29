/* netdrv.c -- см. include/netdrv.h за общей идеей (Фаза 4, подшаг 4). */

#include "netdrv.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "ipc.h"
#include "container.h"
#include "task.h"
#include "kprintf.h"
#include "string.h"

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8139

/* ---- смещения регистров относительно I/O base (BAR0) -- нужны здесь
   только те, что программируются ДО запуска контейнера; остальные
   (IMR/ISR/RCR/TCR/CMD в части RE|TE, CAPR, TSD0) -- уже в
   apps/rtl8139d.c, см. общий комментарий в include/netdrv.h. ---- */
#define REG_MAC0     0x00
#define REG_TSAD0    0x20
#define REG_RBSTART  0x30
#define REG_CMD      0x37
#define REG_CONFIG1  0x52

#define CMD_RST 0x10

/* Полный диапазон портов BAR0 этой карты -- ровно то, что кладём в
   io_len= манифеста контейнера-драйвера (даёт ему доступ и к тем
   регистрам, что программируем здесь, хотя после старта он их уже не
   трогает, и к тем, что программирует сам, см. include/netdrv.h). */
#define RTL_IO_LEN 0x100u

/* Кольцевой RX-буфер: 8 KiB данных + 16 (заголовок статуса первого
   пакета) + 1500 (запас на чтение "через край" при выставленном
   RCR_WRAP) -- то же значение, что было в drivers/rtl8139.c, ДОЛЖНО
   совпадать с RX_BUF_LEN в apps/rtl8139d.c (тот же кусок физической
   памяти, оба конца обязаны одинаково понимать его границы). */
#define RX_BUF_LEN (8192u + 16u + 1500u)
#define RX_BUF_PAGES ((RX_BUF_LEN + 4095u) / 4096u)

/* Каждый из 4 TX-дескрипторов -- на СВОЕЙ странице (проще, чем упаковывать
   несколько 1514-байтных буферов в общую страницу и объяснять
   контейнеру их смещения через shm -- страница и так намного больше,
   чем нужно одному кадру). ДОЛЖНО совпадать с TX_BUF_COUNT/
   TX_SLOT_STRIDE в apps/rtl8139d.c. */
#define TX_BUF_COUNT 4u
#define TX_SLOT_STRIDE 4096u

static bool_t g_found = FALSE;
static u16 g_io_base = 0;
static u8  g_irq_line = 0;

/* Ждёт, пока (inb(port) & mask) не станет want, либо пока не истечёт
   спин-таймаут -- тот же приём, что и ata_wait_status() в
   drivers/ide.c. */
static bool_t spin_until_byte(u16 port, u8 mask, u8 want, u32 max_iters) {
    for (u32 i = 0; i < max_iters; i++) {
        if ((inb(port) & mask) == want) return TRUE;
        io_wait();
    }
    return FALSE;
}

bool_t netdrv_probe(mac_addr_t *out_mac) {
    pci_device_t dev;
    if (!pci_find_device_by_id(RTL_VENDOR_ID, RTL_DEVICE_ID, &dev)) {
        kprintf("net: RTL8139 not found on PCI bus\n");
        return FALSE;
    }

    /* BAR0 отмечен битом 0 == 1 как I/O-пространство; младшие 2 бита --
       флаги, не часть адреса. */
    if (!(dev.bar[0] & 0x1)) {
        kprintf("net: RTL8139 BAR0 is not I/O space, unsupported layout\n");
        return FALSE;
    }
    g_io_base = (u16)(dev.bar[0] & ~0x3u);

    /* Bus mastering + I/O space enable (регистр PCI COMMAND, offset
       0x04) -- без этого карта не сможет сама читать/писать физическую
       память (RBSTART/TSAD*) через DMA. Порты 0xCF8/0xCFC, которыми это
       делается, -- общий ресурс шины, а не диапазон портов этой карты,
       поэтому остаётся здесь, а не в apps/rtl8139d.c (см. общий
       комментарий в include/netdrv.h). */
    u32 pci_cmd = pci_config_read32(dev.bus, dev.device, dev.function, 0x04);
    pci_cmd |= (1u << 2) | (1u << 0);
    pci_config_write32(dev.bus, dev.device, dev.function, 0x04, pci_cmd);

    /* Power on (выводит карту из возможного soft-power-down). */
    outb(g_io_base + REG_CONFIG1, 0x00);

    /* Программный сброс -- ДО RBSTART/TSAD* ниже: сброс возвращает эти
       регистры в состояние по умолчанию, так что программировать их
       нужно строго после, а не до. */
    outb(g_io_base + REG_CMD, CMD_RST);
    if (!spin_until_byte((u16)(g_io_base + REG_CMD), CMD_RST, 0, 1000000u)) {
        kprintf("net: RTL8139 soft reset timed out\n");
        return FALSE;
    }

    /* Читаем "прошитый" MAC-адрес из регистров IDR0..5 (это же
       REG_MAC0) -- контейнеру-драйверу это не понадобится (он не
       заглядывает внутрь кадров), нужно только самому net/net.c. */
    mac_addr_t mac;
    for (int i = 0; i < 6; i++) mac.b[i] = inb(g_io_base + REG_MAC0 + i);

    /* ---- DMA-буферы -- см. общий комментарий в include/netdrv.h за
       тем, почему это должно остаться здесь, а не в apps/rtl8139d.c. ---- */

    void *rx_base = pmm_alloc_frames(RX_BUF_PAGES);
    if (!rx_base) {
        kprintf("net: out of memory for RTL8139 RX ring (%u page(s))\n", (u32)RX_BUF_PAGES);
        return FALSE;
    }
    void *rx_frames[RX_BUF_PAGES];
    for (u32 p = 0; p < RX_BUF_PAGES; p++) {
        rx_frames[p] = (void *)((u8 *)rx_base + p * 4096u);
        memset(rx_frames[p], 0, 4096);
    }
    if (shm_create_from_frames("netdrv-rx", rx_frames, RX_BUF_PAGES, RX_BUF_LEN) < 0) {
        kprintf("net: failed to register RTL8139 RX ring as shm segment\n");
        pmm_free_frames(rx_base, RX_BUF_PAGES);
        return FALSE;
    }

    void *tx_base = pmm_alloc_frames(TX_BUF_COUNT);
    if (!tx_base) {
        kprintf("net: out of memory for RTL8139 TX buffers (%u page(s))\n", TX_BUF_COUNT);
        /* RX-кольцо уже зарегистрировано как shm-сегмент выше -- не
           откатываем: при неудаче здесь netdrv_probe() в любом случае
           вернёт FALSE, net_is_up() останется FALSE и netdrv_launch()
           ничего не запустит, так что "утёкший" shm-сегмент безвреден
           (переживёт до следующей перезагрузки, как и было бы с любым
           другим именованным сегментом без единого держателя). */
        return FALSE;
    }
    void *tx_frames[TX_BUF_COUNT];
    for (u32 p = 0; p < TX_BUF_COUNT; p++) {
        tx_frames[p] = (void *)((u8 *)tx_base + p * TX_SLOT_STRIDE);
        memset(tx_frames[p], 0, TX_SLOT_STRIDE);
    }
    if (shm_create_from_frames("netdrv-tx", tx_frames, TX_BUF_COUNT, TX_BUF_COUNT * TX_SLOT_STRIDE) < 0) {
        kprintf("net: failed to register RTL8139 TX buffers as shm segment\n");
        pmm_free_frames(tx_base, TX_BUF_COUNT);
        return FALSE;
    }

    outl(g_io_base + REG_RBSTART, (u32)(uintptr_t)rx_base);
    for (u32 slot = 0; slot < TX_BUF_COUNT; slot++) {
        outl(g_io_base + REG_TSAD0 + slot * 4, (u32)(uintptr_t)tx_frames[slot]);
    }
    /* IMR/RCR/TCR/CMD сознательно НЕ трогаем -- см. общий комментарий в
       include/netdrv.h: карта должна оставаться выключенной (RE/TE=0,
       IMR=0), пока apps/rtl8139d.c сама их не включит, уже будучи
       готовой обслуживать и IRQ, и netbridge. */

    /* IRQ line, назначенная BIOS/QEMU этому устройству (offset 0x3C
       конфигурационного пространства PCI) -- под QEMU (i440fx + SeaBIOS
       по умолчанию) совпадает с номером "легаси" IRQ на 8259, которым
       уже умеет пользоваться kernel/irqbridge.c. */
    u32 irq_reg = pci_config_read32(dev.bus, dev.device, dev.function, 0x3C);
    u8 irq_line = (u8)(irq_reg & 0xFF);
    if (irq_line == 0xFF || irq_line >= 16) {
        kprintf("net: RTL8139 has no usable legacy IRQ line (got %u)\n", irq_line);
        return FALSE;
    }
    g_irq_line = irq_line;

    g_found = TRUE;
    if (out_mac) *out_mac = mac;

    kprintf("net: RTL8139 at io=0x%x irq=%u mac=%x:%x:%x:%x:%x:%x (driver container pending, see netdrv_launch())\n",
            g_io_base, g_irq_line,
            mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);
    return TRUE;
}

/* Тело выделенного потока ядра, который держит container_spawn() (он
   блокируется до завершения контейнера, см. kernel/container.c) --
   поэтому обязан быть СВОИМ потоком, а не просто вызовом из
   kernel_main(), которому ещё нужно дойти до запуска шелл-контейнера
   (см. Фазу 5, подшаг 5.5, kernel/kernel.c). */
static void netdrv_launch_task(void *arg) {
    (void)arg;

    /* io_base/irq -- десятичными строками через argv, см. общий
       комментарий "Как контейнер узнаёт io_base/irq_line" в
       include/netdrv.h. utoa() -- lib/string.c, общая для ядра и
       userspace (её же использует kernel/container.c: atoi()). */
    char io_base_str[16];
    char irq_str[16];
    utoa((u32)g_io_base, io_base_str, 10);
    utoa((u32)g_irq_line, irq_str, 10);

    container_manifest_t m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, "rtl8139d", CONTAINER_NAME_MAX - 1);
    strncpy(m.path, "/bin/rtl8139d", CONTAINER_PATH_MAX - 1);
    m.caps = CAP_IO | CAP_IRQ | CAP_NET_BRIDGE | CAP_SHM;
    m.io_base = g_io_base;
    m.io_len = RTL_IO_LEN;
    m.irq_line = g_irq_line;

    char *argv[3];
    argv[0] = m.path;
    argv[1] = io_base_str;
    argv[2] = irq_str;

    s32 rc = container_spawn(&m, 3, argv);

    /* В норме сюда не попадаем никогда -- обе роли apps/rtl8139d.c
       (rx/tx, см. её собственный комментарий) крутятся в бесконечном
       цикле. Если всё же попали -- контейнер упал (авария в ring 3,
       см. kernel/isr.c, или явный sys_exit() с ошибкой) -- никакого
       автоматического перезапуска (см. общий стиль этого дерева:
       Фаза 4.4 не пытается быть отказоустойчивой, только рабочей). */
    kprintf("netdrv: rtl8139d container exited (rc=%d) -- networking will not work again until reboot\n", rc);

    for (;;) task_yield(); /* поток ядра не должен просто return'уть, см. netbridge_rx_pump() и другие */
}

void netdrv_launch(void) {
    if (!g_found) return;
    task_create("netdrv-launch", netdrv_launch_task, 0);
}

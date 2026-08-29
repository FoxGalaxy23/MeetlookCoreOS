#include "usb_uhci.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "timer.h"
#include "kprintf.h"

/* ---- Регистры контроллера (I/O space, BAR4) ---- */
#define UHCI_REG_USBCMD    0x00 /* 16 бит */
#define UHCI_REG_USBSTS    0x02 /* 16 бит */
#define UHCI_REG_USBINTR   0x04 /* 16 бит */
#define UHCI_REG_FRNUM     0x06 /* 16 бит */
#define UHCI_REG_FRBASEADD 0x08 /* 32 бита */
#define UHCI_REG_SOFMOD    0x0C /* 8 бит */
#define UHCI_REG_PORTSC1   0x10 /* 16 бит */
#define UHCI_REG_PORTSC2   0x12 /* 16 бит */

#define UHCI_CMD_RS      0x0001 /* Run/Stop */
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004 /* Global Reset */
#define UHCI_CMD_CF      0x0040 /* Configure Flag */
#define UHCI_CMD_MAXP    0x0080 /* Max Packet (1 = 64 байта на кадр для low-speed) */

#define UHCI_PORT_CONNECT        0x0001
#define UHCI_PORT_CONNECT_CHANGE 0x0002
#define UHCI_PORT_ENABLE         0x0004
#define UHCI_PORT_ENABLE_CHANGE  0x0008
#define UHCI_PORT_LOW_SPEED      0x0100
#define UHCI_PORT_RESET          0x0200

/* ---- Указатели расписания (Frame List, QH.link, TD.link) ----
   Общий формат физического указателя на 16-байтную структуру:
   биты 31:4 -- адрес, бит 1 -- Q (1=QH, 0=TD), бит 0 -- T (1=terminate,
   указателя нет). */
#define UHCI_PTR_TERMINATE 0x0001u
#define UHCI_PTR_QH        0x0002u

/* ---- Слово Status/Control TD (см. UHCI 1.1, раздел 3.3.2) ---- */
#define TD_CTRL_ACTLEN_MASK 0x000007FFu
/* Реальная раскладка (сверено построчно с include/hw/usb/uhci-regs.h
   и hw/usb/hcd-uhci.c из исходников QEMU -- у первой версии этого
   файла все биты 18-25 были сдвинуты на 1 позицию вниз, из-за чего
   ACTIVE (23) писался в позицию, которую контроллер читает как STALL
   (22): каждый наш TD выглядел для контроллера как "неактивный,
   застолленный" ещё ДО того, как он вообще пытался его обработать --
   отсюда полное отсутствие каких-либо попыток передачи). Bitstuff
   Error как отдельный бит в этой раскладке отсутствует -- у настоящего
   UHCI (и в модели QEMU) CRC-ошибка и таймаут -- один и тот же бит
   (TIMEOUT, 18). */
#define TD_CTRL_TIMEOUT     (1u << 18) /* CRC-ошибка/таймаут (один бит на оба случая) */
#define TD_CTRL_NAK         (1u << 19)
#define TD_CTRL_BABBLE      (1u << 20)
#define TD_CTRL_STALL       (1u << 22)
#define TD_CTRL_ACTIVE      (1u << 23)
#define TD_CTRL_IOC         (1u << 24)
#define TD_CTRL_IOS         (1u << 25) /* Isochronous Select -- не используем */
#define TD_CTRL_LOWSPEED    (1u << 26)
#define TD_CTRL_CERR_SHIFT  27  /* 2 бита -- счётчик разрешённых ошибок */
#define TD_CTRL_SPD         (1u << 29) /* Short Packet Detect */

#define TD_CTRL_ANY_ERROR (TD_CTRL_TIMEOUT | TD_CTRL_BABBLE | TD_CTRL_STALL)

typedef struct __attribute__((packed, aligned(16))) {
    u32 head_link;    /* горизонтальная ссылка (QHLP) */
    u32 element_link; /* вертикальная ссылка на первый TD очереди (QELP) */
} uhci_qh_t;

typedef struct __attribute__((packed, aligned(16))) {
    u32 link;
    u32 status_ctrl;
    u32 token;
    u32 buffer;
} uhci_td_t;

static u16 g_io_base = 0;
static bool_t g_present = FALSE;

/* Frame List должен быть выровнен по границе 4 KiB (физический адрес
   кратен 4096) -- контроллер требует это аппаратно. QH/TD выравниваем
   по 16 байт (минимум, требуемый форматом указателей, где младшие 4
   бита заняты флагами). Один общий TD достаточен: передачи в этом ядре
   всегда синхронны и последовательны (см. общий комментарий в .h),
   так что второй передаче просто неоткуда взяться, пока не завершится
   первая. */
static u32     g_frame_list[1024] __attribute__((aligned(4096)));
static uhci_qh_t g_qh __attribute__((aligned(16)));
static uhci_td_t g_td __attribute__((aligned(16)));

static void uhci_wait_ms(u32 ms) {
    u32 freq = timer_frequency();
    if (freq == 0) {
        /* Таймер почему-то ещё не запущен -- грубая замена busy-loop'ом,
           лишь бы не заблокироваться навечно (по факту к моменту вызова
           usb_init() из kernel.c таймер уже всегда поднят). */
        for (volatile u32 i = 0; i < ms * 100000u; i++) { }
        return;
    }
    u64 ticks_needed = ((u64)ms * freq) / 1000u;
    if (ticks_needed == 0) ticks_needed = 1;
    u64 deadline = timer_ticks() + ticks_needed;
    while (timer_ticks() < deadline) { }
}

bool_t uhci_init(void) {
    g_present = FALSE;
    g_io_base = 0;

    pci_device_t dev;
    if (!pci_find_device(0x0C, 0x03, 0x00, &dev)) {
        return FALSE; /* нет UHCI-контроллера -- это нормально */
    }

    /* BAR4 у UHCI -- это I/O-space BAR (бит 0 = 1); младшие 2 бита --
       флаги, а не часть адреса. */
    u16 io_base = (u16)(dev.bar[4] & 0xFFFCu);
    if (io_base == 0) return FALSE;
    g_io_base = io_base;

    /* Включаем I/O Space (бит 0) и Bus Master (бит 2) в командном
       регистре PCI -- без первого контроллер не отвечает на порты
       ввода-вывода, без второго не сможет сам читать/писать Frame List
       и буферы передач через DMA. */
    u16 pci_cmd = pci_config_read16(dev.bus, dev.device, dev.function, 0x04);
    pci_cmd |= (1 << 0) | (1 << 2);
    pci_config_write16(dev.bus, dev.device, dev.function, 0x04, pci_cmd);

    /* Глобальный сброс (сигнал сброса на шину, для подключённых
       устройств) -- САМ ПО СЕБЕ не переинициализирует внутренний
       движок расписания контроллера. Для этого нужен ОТДЕЛЬНЫЙ Host
       Controller Reset (HCRESET, бит 1 USBCMD) -- без него FRNUM
       исправно тикает (это независимый счётчик), а вот QH/TD
       контроллер фактически не обрабатывает ни разу, хотя все структуры
       в памяти корректны (проверено вручную через 'xp' в мониторе QEMU
       при отладке -- USBSTS чист, FRNUM растёт, Active в TD не
       снимается). HCRESET сбрасывается контроллером сам, когда сброс
       завершён -- ждём этого (со timeout на случай зависшей эмуляции). */
    outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_GRESET);
    uhci_wait_ms(20);
    outw(io_base + UHCI_REG_USBCMD, 0);
    uhci_wait_ms(1);

    outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    {
        u32 hcreset_timeout = 1000;
        while ((inw(io_base + UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) && --hcreset_timeout) { }
    }

    /* Сбрасываем накопившиеся биты состояния (все они R/WC -- пишем 1,
       чтобы очистить). */
    outw(io_base + UHCI_REG_USBSTS, 0xFFFF);
    outw(io_base + UHCI_REG_USBINTR, 0); /* прерывания не используем -- поллинг */
    outw(io_base + UHCI_REG_FRNUM, 0);

    /* Общая QH: горизонтально никуда не ведёт, вертикально изначально
       пуста (Terminate). Каждый слот Frame List указывает на эту же QH
       (Q=1) -- так контроллер гарантированно заходит в неё на каждом
       кадре, вне зависимости от текущего номера кадра. */
    g_qh.head_link = UHCI_PTR_TERMINATE;
    g_qh.element_link = UHCI_PTR_TERMINATE;

    u32 qh_phys = (u32)(u64)&g_qh;
    for (int i = 0; i < 1024; i++) {
        g_frame_list[i] = qh_phys | UHCI_PTR_QH;
    }

    outl(io_base + UHCI_REG_FRBASEADD, (u32)(u64)g_frame_list);

    /* Запуск: Run/Stop=1, Configure Flag=1 (нужен некоторым контроллерам,
       чтобы вообще замечать подключение устройств), Max Packet=1 (64
       байта разрешены на low-speed-кадр -- иначе часть low-speed
       клавиатур/мышей с полноразмерными interrupt-пакетами не будет
       обслужена). */
    outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    uhci_wait_ms(10);

    g_present = TRUE;
    kprintf("USB: контроллер UHCI найден (io=0x%x).\n", io_base);
    return TRUE;
}

u32 uhci_port_count(void) {
    return g_present ? 2 : 0; /* стандартный UHCI: ровно 2 порта корневого хаба */
}

static u16 uhci_port_reg(u32 port) {
    return (port == 0) ? UHCI_REG_PORTSC1 : UHCI_REG_PORTSC2;
}

bool_t uhci_port_is_connected(u32 port) {
    if (!g_present || port >= 2) return FALSE;
    return (inw(g_io_base + uhci_port_reg(port)) & UHCI_PORT_CONNECT) != 0;
}

bool_t uhci_port_reset_and_enable(u32 port, bool_t *out_low_speed) {
    if (!g_present || port >= 2) return FALSE;
    u16 reg = g_io_base + uhci_port_reg(port);

    /* Удерживаем Port Reset не менее 50 мс (USB 2.0, 7.1.7.5 -- время
       сброса корневого порта), затем снимаем. */
    outw(reg, UHCI_PORT_RESET);
    uhci_wait_ms(50);
    outw(reg, inw(reg) & (u16)~UHCI_PORT_RESET);
    uhci_wait_ms(10);

    /* Сбрасываем биты изменения состояния (R/WC) и включаем порт. */
    u16 v = inw(reg);
    v |= UHCI_PORT_CONNECT_CHANGE | UHCI_PORT_ENABLE_CHANGE | UHCI_PORT_ENABLE;
    outw(reg, v);
    uhci_wait_ms(10);

    v = inw(reg);
    if (!(v & UHCI_PORT_ENABLE)) {
        /* Некоторые (виртуальные) контроллеры включают порт не сразу --
           пробуем ещё раз явно поднять бит Enable. */
        outw(reg, v | UHCI_PORT_ENABLE);
        uhci_wait_ms(10);
        v = inw(reg);
    }

    if (out_low_speed) *out_low_speed = (v & UHCI_PORT_LOW_SPEED) != 0;
    return (v & UHCI_PORT_ENABLE) != 0;
}

s32 uhci_transfer_packet(u8 dev_addr, u8 endpoint, u8 pid, bool_t low_speed,
                          void *buf, u16 len, u8 *toggle, u32 timeout_ms) {
    if (!g_present) return -1;

    u32 max_len_field = (len == 0) ? 0x7FFu : (u32)(len - 1);

    g_td.link = UHCI_PTR_TERMINATE;
    g_td.status_ctrl = TD_CTRL_ACTIVE
                      | (3u << TD_CTRL_CERR_SHIFT) /* до 3 аппаратных повторов при реальной ошибке */
                      | TD_CTRL_SPD
                      | (low_speed ? TD_CTRL_LOWSPEED : 0);
    g_td.token = (u32)pid
               | ((u32)(dev_addr & 0x7Fu) << 8)
               | ((u32)(endpoint & 0x0Fu) << 15)
               | ((u32)(*toggle & 1u) << 19)
               | (max_len_field << 21);
    g_td.buffer = (len == 0) ? 0 : (u32)(u64)buf;

    /* Публикуем TD в расписании -- со следующего же кадра (максимум
       через ~1 мс) контроллер его увидит через общую QH. При NAK от
       устройства контроллер (аппаратно, без нашего участия) повторяет
       транзакцию на следующих кадрах сам, не трогая C_ERR и не снимая
       Active -- поэтому одного бита Active достаточно как признака
       "ещё не готово", а таймаут ниже нужен только на случай, если
       устройство вообще не отвечает. */
    g_qh.element_link = ((u32)(u64)&g_td); /* Q=0 (это TD, не QH), T=0 */

    u32 freq = timer_frequency();
    u64 ticks_needed = freq ? (((u64)timeout_ms * freq) / 1000u) : 0;
    if (ticks_needed == 0) ticks_needed = 1;
    u64 deadline = timer_ticks() + ticks_needed;

    while ((g_td.status_ctrl & TD_CTRL_ACTIVE) && timer_ticks() < deadline) { }

    /* Снимаем TD с расписания вне зависимости от исхода -- иначе
       контроллер продолжит его "видеть" (хоть уже неактивным) на каждом
       следующем кадре без всякой пользы. */
    g_qh.element_link = UHCI_PTR_TERMINATE;

    if (g_td.status_ctrl & TD_CTRL_ACTIVE) return -1;    /* таймаут (устройство не ответило) */
    if (g_td.status_ctrl & TD_CTRL_ANY_ERROR) return -1; /* STALL/CRC-timeout/babble */

    u32 actlen_field = g_td.status_ctrl & TD_CTRL_ACTLEN_MASK;
    u32 actual = (actlen_field == 0x7FFu) ? 0u : (actlen_field + 1u);

    *toggle ^= 1u;
    return (s32)actual;
}

#ifndef NET_H
#define NET_H

#include "types.h"

/* net.h -- общие типы и служебные функции сетевого стека MeetlookCoreOS.
 *
 * Общая архитектура (снизу вверх, см. соответствующие заголовки):
 *
 *   apps/rtl8139d.c (+   -- физический уровень: карта RTL8139 по PCI,
 *   kernel/netdrv.c,        приём/передача сырых Ethernet-кадров. С Фазы 4,
 *   kernel/netbridge.c)     подшага 4 (см. CONTAINER_ARCHITECTURE.md) сам
 *                          драйвер -- обычный ring-3 контейнер, а не код
 *                          ядра: kernel/netdrv.c оставляет в ядре только то,
 *                          что нельзя сделать из ring 3 (обход шины PCI,
 *                          физические адреса DMA-буферов), kernel/netbridge.c
 *                          -- канал кадров между ним и остальным стеком.
 *   net/eth.c   (eth.h)  -- разбор/сборка заголовка Ethernet, диспетчеризация
 *                          по EtherType (ARP -> arp.c, IPv4 -> ip.c).
 *   net/arp.c   (arp.h)  -- таблица соответствия IPv4 <-> MAC, ARP-запросы.
 *   net/ip.c    (ip.h)   -- заголовок IPv4, контрольная сумма, диспетчеризация
 *                          по протоколу (ICMP -> icmp.c, UDP -> udp.c,
 *                          TCP -> tcp.c).
 *   net/icmp.c  (icmp.h) -- ICMP echo request/reply (используется 'ping').
 *   net/udp.c   (udp.h)  -- UDP, используется DHCP- и DNS-клиентами.
 *   net/dhcp.c, net/dns.c -- клиенты поверх UDP.
 *   net/tcp.c   (tcp.h)  -- минимальный клиентский TCP (активное открытие),
 *                          используется 'wget'/'curl'.
 *
 * Модель конкурентности: до Фазы 4.4 карта работала по прерываниям (IRQ)
 * ПРЯМО в ядре, и весь разбор входящего кадра (Ethernet -> ARP/IP ->
 * ICMP/UDP/TCP) происходил СИНХРОННО внутри обработчика прерывания, ровно
 * как обработка нажатий клавиш в drivers/keyboard.c. Теперь драйвер --
 * контейнер (apps/rtl8139d.c), а разбор входящего кадра происходит в
 * ОБЫЧНОМ потоке ядра netbridge_rx_pump() (kernel/netbridge.c), который
 * блокируется в ожидании кадров от контейнера и вызывает eth_input() уже
 * из task-контекста, а не из настоящего прерывания -- сам обработчик
 * прерывания на стороне ядра теперь не знает о картах вообще ничего (см.
 * kernel/irqbridge.c). Блокирующие пользовательские вызовы (ping, resolve,
 * tcp_connect/recv, ...) не спят на условной переменной, а опрашивают
 * нужное состояние короткими интервалами через task_sleep() (см.
 * include/task.h) -- это проще, чем
 * заводить отдельный mutex_t/condvar_t на каждую сущность, и вполне
 * достаточно для целей этой ОС (задержки в единицы миллисекунд, не
 * критичные для настоящего сетевого стека). Общие структуры (ARP-кэш,
 * таблица TCP-соединений, буферы UDP-ответов) защищены короткими
 * критическими секциями через net_irq_save()/net_irq_restore() -- тем же
 * приёмом, что уже используется в kernel/ipc.c. */

typedef struct __attribute__((packed)) {
    u8 b[6];
} mac_addr_t;

extern const mac_addr_t MAC_BROADCAST;
extern const mac_addr_t MAC_ZERO;

static inline bool_t mac_equal(mac_addr_t a, mac_addr_t b) {
    for (int i = 0; i < 6; i++) if (a.b[i] != b.b[i]) return FALSE;
    return TRUE;
}

/* IPv4-адрес хранится в ХОСТОВОМ порядке байт везде в этом API (как
   обычный u32, например 0xC0A80001 == 192.168.0.1) -- преобразование в
   сетевой порядок происходит только непосредственно при сборке пакета
   (см. net_htons()/net_htonl() ниже) и обратно при разборе. Так все
   вычисления (сравнения, маски подсети) выглядят естественно. */
typedef u32 ip_addr_t;

#define IP_NONE 0u
#define IP_BROADCAST 0xFFFFFFFFu

static inline ip_addr_t ip_make(u8 a, u8 b, u8 c, u8 d) {
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d;
}

/* ---- порядок байт: x86_64 всегда little-endian, сеть -- всегда
   big-endian ("network byte order"), поэтому здесь просто разворот байт,
   без проверки архитектуры (в этом ядре другой и не бывает). ---- */
static inline u16 net_htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 net_ntohs(u16 v) { return net_htons(v); }
static inline u32 net_htonl(u32 v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline u32 net_ntohl(u32 v) { return net_htonl(v); }

/* Интернет-контрольная сумма (RFC 1071) -- используется IPv4/ICMP/UDP/TCP.
   data/len -- в СЕТЕВОМ порядке байт как есть (то есть буфер ровно таким,
   каким он уйдёт/пришёл по проводу); возвращает готовое значение, уже
   развёрнутое в сетевой порядок байт (см. net_checksum_finish() в
   net/net.c) -- его можно положить ПРЯМЫМ присваиванием в поле
   структуры ("hdr.checksum = net_checksum(...)"), без отдельного
   net_htons() на месте вызова, и так же прочитать обратно при проверке
   ("net_checksum(pkt_with_checksum_field, len) == 0" для валидного
   пакета). */
u16 net_checksum(const void *data, u32 len);

/* net_checksum(), которую можно "продолжить" вторым куском данных --
   нужна для псевдозаголовка UDP/TCP (сумма считается сразу над
   псевдозаголовком и телом сегмента, но они лежат в разных буферах). */
u32 net_checksum_partial(u32 acc, const void *data, u32 len);
u16 net_checksum_finish(u32 acc);

/* ---- короткие критические секции для общих таблиц стека (ARP-кэш,
   TCP-соединения, однобуферные UDP-ответы DNS/DHCP) -- см. общий
   комментарий выше и kernel/ipc.c за тем же приёмом. ---- */
u64 net_irq_save(void);
void net_irq_restore(u64 flags);

/* ---- состояние единственного сетевого интерфейса ----
 * Эта ОС поддерживает ровно один физический интерфейс (см. задание:
 * "работает по Ethernet") -- отдельной структуры netif_t с списком
 * интерфейсов сознательно нет, вся конфигурация -- несколько глобальных
 * переменных в net/net.c, к которым обращаются через функции ниже. */
typedef struct {
    bool_t   up;          /* карта найдена и инициализирована */
    mac_addr_t mac;
    ip_addr_t ip;
    ip_addr_t netmask;
    ip_addr_t gateway;
    ip_addr_t dns;
    bool_t   dhcp_bound;  /* TRUE, если ip/netmask/gateway/dns получены по DHCP */
} net_config_t;

/* Инициализирует весь стек: обнаруживает карту на шине PCI и программирует
   её DMA-буферы (netdrv_probe(), kernel/netdrv.c -- см. include/netdrv.h за
   тем, почему это ещё в ядре, а не в контейнере), поднимает Ethernet/ARP/
   IP/ICMP/UDP/TCP уровни. Сам контейнер-драйвер (apps/rtl8139d.c) здесь
   ЕЩЁ не запускается -- ему нужна файловая система, см. netdrv_launch()
   (include/netdrv.h), вызывается отдельно, после vfs_init() (kernel/
   kernel.c). Конфигурация IP по умолчанию не назначается -- см.
   net_configure_static()/net_dhcp_request() (kernel/shell.c команда
   'ifconfig'/'dhcp', либо SYS_NET_DHCP из userspace).
   Ничего не делает (net_is_up() == FALSE), если PCI-карта не найдена --
   остальной стек в этом случае просто ничего не отправляет. */
void net_init(void);

bool_t net_is_up(void);
void net_get_config(net_config_t *out);
void net_set_static(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway, ip_addr_t dns);
/* Как net_set_static(), но помечает конфигурацию как полученную по DHCP
   (net_config_t::dhcp_bound) -- используется только net/dhcp.c. */
void net_set_dhcp_result(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway, ip_addr_t dns);
mac_addr_t net_my_mac(void);
ip_addr_t net_my_ip(void);

/* TRUE, если ip принадлежит локальной подсети (ip & netmask == our_ip &
   netmask) -- используется ip.c, чтобы решить, слать ли ARP-запрос
   напрямую на ip или на шлюз (net_config.gateway). */
bool_t net_ip_is_local(ip_addr_t ip);

#endif

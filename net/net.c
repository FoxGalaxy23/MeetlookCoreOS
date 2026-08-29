/* net.c -- см. include/net.h за общим описанием стека. */

#include "net.h"
#include "netdrv.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "kprintf.h"

const mac_addr_t MAC_BROADCAST = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
const mac_addr_t MAC_ZERO      = { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

static net_config_t g_cfg;

u64 net_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void net_irq_restore(u64 flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

u32 net_checksum_partial(u32 acc, const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    while (len > 1) {
        acc += ((u32)p[0] << 8) | (u32)p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        acc += (u32)p[0] << 8;
    }
    return acc;
}

u16 net_checksum_finish(u32 acc) {
    while (acc >> 16) {
        acc = (acc & 0xFFFFu) + (acc >> 16);
    }
    u16 host_value = (u16)(~acc & 0xFFFFu);
    /* net_checksum_partial() трактует каждую пару байт как BIG-ENDIAN
       16-битное слово (p[0] -- старший байт) -- ровно так лежит поле в
       буфере пакета на проводе. Но у ВСЕХ вызывающих сайтов (net/ip.c,
       net/icmp.c, net/udp.c, net/tcp.c) результат этой функции кладётся
       в поле структуры ПРЯМЫМ присваиванием ("hdr.checksum = ..."), а
       x86 (little-endian) при таком присваивании кладёт МЛАДШИЙ байт
       по МЕНЬШЕМУ адресу -- ровно наоборот. Без разворота здесь исходящая
       контрольная сумма во всех протоколах была бы неверной (проверено
       эмпирически: пакет не проходит даже собственную верификацию), и
       любой настоящий стек-получатель (включая QEMU slirp) её бы просто
       отбрасывал -- то есть сеть работала бы только "по счастливой
       случайности" совпадения байт. net_htons() -- ровно нужный разворот
       байт (см. include/net.h). */
    return net_htons(host_value);
}

u16 net_checksum(const void *data, u32 len) {
    return net_checksum_finish(net_checksum_partial(0, data, len));
}

void net_init(void) {
    g_cfg.up = FALSE;
    g_cfg.ip = IP_NONE;
    g_cfg.netmask = IP_NONE;
    g_cfg.gateway = IP_NONE;
    g_cfg.dns = IP_NONE;
    g_cfg.dhcp_bound = FALSE;

    /* Обнаружение карты (PCI, DMA-буферы) -- kernel/netdrv.c, Фаза 4,
       подшаг 4. Собственно драйвер (кольцо приёма, TSD/TOK при передаче,
       обработчик IRQ) -- уже не здесь и не в ядре вообще, а в
       apps/rtl8139d.c, запускается отдельно как контейнер, ПОСЛЕ того,
       как поднимется VFS -- см. netdrv_launch(), kernel/kernel.c. */
    mac_addr_t mac;
    if (!netdrv_probe(&mac)) {
        kprintf("net: no network card found -- networking disabled\n");
        return;
    }

    g_cfg.mac = mac;
    g_cfg.up = TRUE;
    arp_init();
    icmp_init();
    udp_init();
    tcp_init();

    kprintf("net: interface up, no IP configured yet -- use 'ifconfig' or 'dhcp'\n");
}

bool_t net_is_up(void) {
    return g_cfg.up;
}

void net_get_config(net_config_t *out) {
    *out = g_cfg;
}

void net_set_static(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway, ip_addr_t dns) {
    g_cfg.ip = ip;
    g_cfg.netmask = netmask;
    g_cfg.gateway = gateway;
    g_cfg.dns = dns;
    g_cfg.dhcp_bound = FALSE;
}

void net_set_dhcp_result(ip_addr_t ip, ip_addr_t netmask, ip_addr_t gateway, ip_addr_t dns) {
    g_cfg.ip = ip;
    g_cfg.netmask = netmask;
    g_cfg.gateway = gateway;
    g_cfg.dns = dns;
    g_cfg.dhcp_bound = TRUE;
}

mac_addr_t net_my_mac(void) {
    return g_cfg.mac;
}

ip_addr_t net_my_ip(void) {
    return g_cfg.ip;
}

bool_t net_ip_is_local(ip_addr_t ip) {
    if (g_cfg.netmask == 0) return FALSE;
    return (ip & g_cfg.netmask) == (g_cfg.ip & g_cfg.netmask);
}

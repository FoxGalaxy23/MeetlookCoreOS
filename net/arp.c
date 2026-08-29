/* arp.c -- см. include/arp.h за общим описанием. */

#include "arp.h"
#include "eth.h"
#include "string.h"
#include "task.h"

#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4      0x0800u
#define ARP_OP_REQUEST      1u
#define ARP_OP_REPLY        2u

typedef struct __attribute__((packed)) {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 oper;
    mac_addr_t sha; /* sender hardware address */
    u32 spa;        /* sender protocol address (сетевой порядок байт) */
    mac_addr_t tha; /* target hardware address */
    u32 tpa;        /* target protocol address (сетевой порядок байт) */
} arp_packet_t;

#define ARP_CACHE_SIZE 16u

typedef struct {
    bool_t valid;
    ip_addr_t ip;
    mac_addr_t mac;
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
static u32 g_next_evict = 0;

void arp_init(void) {
    for (u32 i = 0; i < ARP_CACHE_SIZE; i++) g_cache[i].valid = FALSE;
    g_next_evict = 0;
}

static bool_t cache_lookup(ip_addr_t ip, mac_addr_t *out) {
    u64 f = net_irq_save();
    for (u32 i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            *out = g_cache[i].mac;
            net_irq_restore(f);
            return TRUE;
        }
    }
    net_irq_restore(f);
    return FALSE;
}

static void cache_insert(ip_addr_t ip, mac_addr_t mac) {
    u64 f = net_irq_save();
    for (u32 i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            g_cache[i].mac = mac;
            net_irq_restore(f);
            return;
        }
    }
    u32 slot = 0;
    bool_t found_free = FALSE;
    for (u32 i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) { slot = i; found_free = TRUE; break; }
    }
    if (!found_free) {
        slot = g_next_evict;
        g_next_evict = (g_next_evict + 1) % ARP_CACHE_SIZE;
    }
    g_cache[slot].valid = TRUE;
    g_cache[slot].ip = ip;
    g_cache[slot].mac = mac;
    net_irq_restore(f);
}

static void send_arp(u16 oper, ip_addr_t target_ip, mac_addr_t target_mac, mac_addr_t dst_eth) {
    arp_packet_t pkt;
    pkt.htype = net_htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = net_htons(ARP_PTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = net_htons(oper);
    pkt.sha = net_my_mac();
    pkt.spa = net_htonl(net_my_ip());
    pkt.tha = target_mac;
    pkt.tpa = net_htonl(target_ip);
    eth_send(dst_eth, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

void arp_input(const u8 *payload, u32 len) {
    if (len < sizeof(arp_packet_t)) return;
    arp_packet_t pkt;
    memcpy(&pkt, payload, sizeof(pkt));

    if (net_ntohs(pkt.htype) != ARP_HTYPE_ETHERNET || net_ntohs(pkt.ptype) != ARP_PTYPE_IPV4) return;

    ip_addr_t sender_ip = net_ntohl(pkt.spa);
    u16 oper = net_ntohs(pkt.oper);

    /* Любой ARP-пакет (запрос ИЛИ ответ) с осмысленным sender IP пополняет
       кэш -- это "gratuitous learning", стандартное поведение реальных
       стеков: не нужно ждать собственного запроса, чтобы узнать соседа. */
    if (sender_ip != IP_NONE) {
        cache_insert(sender_ip, pkt.sha);
    }

    if (oper == ARP_OP_REQUEST && net_ntohl(pkt.tpa) == net_my_ip() && net_my_ip() != IP_NONE) {
        send_arp(ARP_OP_REPLY, sender_ip, pkt.sha, pkt.sha);
    }
}

bool_t arp_resolve(ip_addr_t ip, mac_addr_t *out_mac, u32 timeout_ms) {
    if (ip == IP_BROADCAST) {
        *out_mac = MAC_BROADCAST;
        return TRUE;
    }
    if (cache_lookup(ip, out_mac)) return TRUE;
    if (!net_is_up() || net_my_ip() == IP_NONE) return FALSE;

    send_arp(ARP_OP_REQUEST, ip, MAC_ZERO, MAC_BROADCAST);

    u32 waited = 0;
    while (waited < timeout_ms) {
        task_sleep(10);
        waited += 10;
        if (cache_lookup(ip, out_mac)) return TRUE;
        if ((waited % 300) == 0) {
            /* Повторяем запрос -- первый мог потеряться (обычное дело в
               реальной сети, и вполне возможно даже в QEMU при большой
               нагрузке на очередь IRQ). */
            send_arp(ARP_OP_REQUEST, ip, MAC_ZERO, MAC_BROADCAST);
        }
    }
    return FALSE;
}

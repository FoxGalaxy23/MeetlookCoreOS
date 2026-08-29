#ifndef ARP_H
#define ARP_H

#include "types.h"
#include "net.h"

/* arp.h -- Address Resolution Protocol (RFC 826): отображение IPv4 ->
 * MAC в локальном сегменте Ethernet. Кэш -- маленькая статическая
 * таблица (без вытеснения по LRU, просто перезаписывает самую старую
 * запись при переполнении -- этого достаточно для учебной ОС с
 * единственным интерфейсом и типичной домашней/QEMU-сетью из нескольких
 * узлов). */

void arp_init(void);

/* Вызывается eth_input() для кадров с EtherType == ETHERTYPE_ARP.
   Обрабатывает и запросы (отвечает, если запрашивают наш IP), и ответы
   (пополняет кэш и, если кто-то ждёт именно этот адрес в arp_resolve(),
   даёт им увидеть результат на следующей проверке). */
void arp_input(const u8 *payload, u32 len);

/* Ищет MAC для ip в кэше; при отсутствии -- рассылает ARP-запрос и ждёт
   ответа (опрос с task_sleep(), см. общий комментарий в include/net.h),
   не дольше timeout_ms. Возвращает TRUE и заполняет *out_mac при успехе. */
bool_t arp_resolve(ip_addr_t ip, mac_addr_t *out_mac, u32 timeout_ms);

#endif

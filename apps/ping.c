/* ping.c -- аналог стандартной утилиты ping: ICMP echo request/reply
 * поверх собственного TCP/IP-стека ядра (см. include/icmp.h,
 * SYS_NET_PING в include/sysabi.h). Работает по Ethernet через RTL8139,
 * начиная с Фазы 4.4 (см. CONTAINER_ARCHITECTURE.md) -- обычный
 * контейнер-драйвер (apps/rtl8139d.c), а не код ядра -- других
 * физических уровней эта ОС не знает.
 *
 * В отличие от настоящего ping, не выдерживает ровно секундную паузу
 * между запросами: SYS_NET_PING сам по себе блокируется до получения
 * ответа (или таймаута), так что последовательные вызовы и так разнесены
 * во времени примерно на RTT -- отдельного sys_sleep() в этой ОС нет
 * (см. include/usyscall.h), а плотный busy-loop без вызова, который так
 * или иначе блокируется, здесь неуместен. */

#include "mlapp.h"
#include "nethelp.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    if (argc < 2) {
        net_put("usage: ping <host> [count]\n");
        sys_exit(1);
    }

    u32 count = 4;
    if (argc >= 3) {
        s32 c = atoi(argv[2]);
        if (c > 0) count = (u32)c;
    }

    u32 ip;
    if (!net_resolve_or_print_error(argv[1], &ip)) sys_exit(1);

    net_put("PING "); net_put(argv[1]); net_put(" (");
    net_put_ip(ip); net_put("): 32 data bytes\n");

    u32 sent = 0, received = 0;
    u32 min_rtt = 0, max_rtt = 0, sum_rtt = 0;

    for (u32 i = 0; i < count; i++) {
        sent++;
        u32 rtt;
        if (sys_net_ping(ip, 2000, &rtt) == 0) {
            received++;
            sum_rtt += rtt;
            if (received == 1 || rtt < min_rtt) min_rtt = rtt;
            if (rtt > max_rtt) max_rtt = rtt;

            net_put("32 bytes from "); net_put_ip(ip);
            net_put(": seq="); net_put_u32(i);
            net_put(" time="); net_put_u32(rtt); net_put("ms\n");
        } else {
            net_put("Request timeout for seq "); net_put_u32(i); net_put("\n");
        }
    }

    net_put("\n--- "); net_put(argv[1]); net_put(" ping statistics ---\n");
    net_put_u32(sent); net_put(" packets transmitted, ");
    net_put_u32(received); net_put(" received, ");
    net_put_u32(sent > 0 ? (sent - received) * 100 / sent : 0);
    net_put("% packet loss\n");

    if (received > 0) {
        net_put("round-trip min/avg/max = ");
        net_put_u32(min_rtt); net_put("/");
        net_put_u32(sum_rtt / received); net_put("/");
        net_put_u32(max_rtt); net_put(" ms\n");
    }

    sys_exit(received > 0 ? 0 : 1);
    return 0; /* не достигается */
}

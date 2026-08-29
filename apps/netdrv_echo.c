/* netdrv_echo.c -- демонстрация Фазы 4, подшаг 3 (см.
 * CONTAINER_ARCHITECTURE.md, include/netbridge.h): контейнер, играющий
 * роль будущего драйвера-контейнера сетевой карты (Фаза 4, подшаг 4 --
 * ещё не сделан), но БЕЗ настоящего железа -- просто эхо: забирает
 * "кадр на отправку" из TX-очереди (то, что в 4.4 положил бы туда
 * eth_send() вместо прямого вызова rtl8139_send()), печатает его и
 * кладёт ТЕ ЖЕ байты обратно в RX-очередь ("кадр, который только что
 * пришёл") -- ровно то, что сделала бы физическая петля (loopback) на
 * настоящей сетевой карте.
 *
 * Нужен CAP_NET_BRIDGE (см. rootfs/containers/netdrv-echo.manifest) --
 * обычный CAP_NET (curl/wget/ping) для этого не годится и не должен:
 * это privileged-доступ к сырым кадрам в обход всего стека (net/eth.c).
 *
 * Как попробовать (сначала кладём кадры в TX-очередь ИЗ ШЕЛЛА -- 'nbtest',
 * см. shell/shell.c, -- ОНИ ОСТАЮТСЯ там, дожидаясь потребителя, ring
 * buffer на NETBRIDGE_QUEUE_SLOTS слотов, см. include/netbridge.h --
 * порядок команд поэтому не важен, можно и сначала runc, тогда
 * netdrv_echo просто заблокируется на sys_netbridge_tx_dequeue() до
 * первого 'nbtest'):
 *
 *   > nbtest hello-from-shell
 *   > nbtest another-test-frame
 *   > runc /containers/netdrv-echo.manifest
 *   [netdrv_echo вычитывает оба кадра из TX-очереди, печатает их и
 *    кладёт обратно в RX-очередь; отдельный поток ядра netbridge-rx
 *    (kernel/kernel.c) забирает их оттуда и печатает
 *    "netbridge: rx pump got N byte(s) ..." для каждого, затем зовёт
 *    eth_input() -- см. общий комментарий в include/netbridge.h про то,
 *    почему это безопасно даже на кадрах, не являющихся настоящим Ethernet] */

#include "mlapp.h"
#include "libc.h"

#define NUM_ROUNDS 5u

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;
    u8 buf[1514];

    printf("netdrv_echo: waiting for frames on the TX bridge queue "
           "(shell: 'nbtest <text>' feeds one in) ...\n");

    for (u32 i = 0; i < NUM_ROUNDS; i++) {
        s32 len = sys_netbridge_tx_dequeue(buf, sizeof(buf));
        if (len < 0) {
            printf("netdrv_echo: sys_netbridge_tx_dequeue failed, rc=%d "
                   "(SYSCALL_ERR_PERM=-5 -- no CAP_NET_BRIDGE in this container's manifest)\n", len);
            sys_exit(1);
        }

        printf("netdrv_echo: round %u/%u -- dequeued %d byte(s) from TX queue, echoing to RX queue\n",
               i + 1, (u32)NUM_ROUNDS, len);

        s32 rc = sys_netbridge_rx_enqueue(buf, (u32)len);
        if (rc < 0) {
            printf("netdrv_echo: sys_netbridge_rx_enqueue failed, rc=%d (RX queue full?)\n", rc);
        }
    }

    printf("netdrv_echo: done -- %u frame(s) echoed through the kernel<->driver-container bridge\n",
           (u32)NUM_ROUNDS);
    sys_exit(0);
    return 0;
}

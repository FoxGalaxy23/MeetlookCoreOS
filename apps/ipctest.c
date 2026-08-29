/* ipctest.c -- демонстрация всех трёх примитивов IPC (include/ipc.h)
 * вместе: пайп используется как сигнал синхронизации (сама по себе
 * разделяемая память никак не сообщает противоположной стороне, ЧТО и
 * КОГДА можно читать -- этим и занимается пайп в этом примере), очередь
 * сообщений -- как канал с текстовым сообщением с сохранёнными
 * границами (в отличие от пайпа, где нет понятия "одно сообщение"),
 * разделяемая память -- как общий счётчик, который родитель и потомок
 * оба видят и меняют напрямую, без единого системного вызова на каждое
 * обращение.
 *
 * Порядок событий:
 *   1. родитель создаёт пайп, очередь сообщений и сегмент разделяемой
 *      памяти и присоединяет сегмент К СЕБЕ -- всё это ДО fork(), чтобы
 *      потомок унаследовал те же самые хендлы (см. include/ipc.h:
 *      ipc_task_fork_copy(), и особый случай для shm в as_clone_cow(),
 *      kernel/paging.c -- сегмент остаётся ДЕЙСТВИТЕЛЬНО общим, а не
 *      становится приватной copy-on-write копией при первой записи);
 *   2. fork();
 *   3. потомок: увеличивает общий счётчик в shm, кладёт текстовое
 *      сообщение в очередь, пишет один байт в пайп ("готово") и
 *      завершается -- всё, что потомок не закрыл явно (пишущий конец
 *      пайпа, свою очередь, своё присоединение к shm), сама закроет
 *      ipc_task_cleanup() (kernel/ipc.c) при его завершении;
 *   4. родитель: блокируется на чтении пайпа, дожидаясь байта от
 *      потомка, затем читает сообщение из очереди и читает итоговое
 *      значение общего счётчика -- напрямую, как из обычной памяти,
 *      без какого-либо системного вызова. */

#include "mlapp.h"
#include "usyscall.h"
#include "string.h"

static void put(const char *s) {
    sys_write(s, strlen(s));
}

static void put_u32(u32 v) {
    char buf[16];
    utoa(v, buf, 10);
    put(buf);
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    put("ipctest: creating pipe, message queue, shared memory...\n");

    s32 fds[2];
    if (sys_pipe_create(fds) < 0) {
        put("ipctest: sys_pipe_create failed\n");
        sys_exit(-1);
    }

    s32 mqh = sys_mq_create(0); /* анонимная очередь -- достаточно наследования через fork() */
    if (mqh < 0) {
        put("ipctest: sys_mq_create failed\n");
        sys_exit(-1);
    }

    s32 shm_id = sys_shm_create(0, 4096); /* анонимный сегмент, 4 KiB */
    if (shm_id < 0) {
        put("ipctest: sys_shm_create failed\n");
        sys_exit(-1);
    }

    void *shm_vaddr = 0;
    if (sys_shm_attach(shm_id, &shm_vaddr) < 0) {
        put("ipctest: sys_shm_attach failed\n");
        sys_exit(-1);
    }
    s32 *counter = (s32 *)shm_vaddr;
    *counter = 0; /* родитель инициализирует общий счётчик ДО fork() */

    s32 pid = sys_fork();
    if (pid < 0) {
        put("ipctest: fork() failed\n");
        sys_exit(-1);
    }

    if (pid == 0) {
        /* ---- потомок ---- */
        sys_pipe_close(fds[0]); /* потомок только пишет в пайп -- закрываем свой читающий конец */

        *counter += 42; /* физически общая страница -- см. as_clone_cow() (kernel/paging.c) */

        static const char msg[] = "hello from child, via message queue";
        sys_mq_send(mqh, msg, sizeof(msg) - 1);

        sys_pipe_write(fds[1], "!", 1); /* сигнал родителю: всё готово, можно читать */

        sys_exit(0); /* остальное (fds[1], mqh, присоединение к shm) закроет ipc_task_cleanup() */
    }

    /* ---- родитель ---- */
    sys_pipe_close(fds[1]); /* родитель только читает пайп -- закрываем свой пишущий конец */

    char sync_byte;
    sys_pipe_read(fds[0], &sync_byte, 1); /* блокируется до записи потомка (или EOF, если он упал) */
    put("ipctest: got sync byte from child via pipe\n");

    char msgbuf[64];
    s32 n = sys_mq_recv(mqh, msgbuf, sizeof(msgbuf) - 1);
    if (n >= 0) {
        msgbuf[n] = '\0';
        put("ipctest: message queue says: ");
        put(msgbuf);
        put("\n");
    }

    put("ipctest: shared counter after child's write = ");
    put_u32((u32)*counter); /* должно быть 42 -- настоящая общая память, не копия */
    put("\n");

    sys_pipe_close(fds[0]);
    sys_mq_close(mqh);
    sys_shm_detach(shm_vaddr);
    sys_shm_destroy(shm_id);

    sys_exit(0);
    return 0; /* не достигается */
}

#ifndef SYSABI_H
#define SYSABI_H

#include "types.h"

/* sysabi.h -- номера системных вызовов MeetlookCoreOS и общая часть
 * ABI. Единственный заголовок, который включают ОБЕ стороны:
 *   - ядро: kernel/syscall.c (диспетчер, см. syscall_isr_handler);
 *   - sandboxed-приложения: include/usyscall.h / lib/usyscall.c
 *     (обёртки над `int 0x80`, линкуются в каждый ring3-бинарник).
 *
 * Соглашение о вызове: программное прерывание `int 0x80`.
 *   rax = номер syscall (SYS_* ниже)
 *   rdi, rsi, rdx, rcx = аргументы 1..4
 *   возврат -- в rax, как s64 (отрицательные значения -- код ошибки,
 *              см. SYSCALL_ERR_* ниже)
 * В отличие от быстрой пары syscall/sysret (MSR-based), `int 0x80`
 * само по себе не требует настройки MSR (STAR/LSTAR/FMASK) и не
 * трогает rcx/r11 особым образом -- решение сознательно в пользу
 * простоты и прозрачности для учебной ОС; ничто не мешает добавить
 * syscall/sysret отдельным, более быстрым путём позже, оставив int 0x80
 * как медленный, но всегда работающий запасной вариант.
 *
 * НИ ОДИН из аргументов-указателей не считается доверенным просто
 * потому, что лежит в регистре -- см. include/uaccess.h за тем, как
 * kernel/syscall.c проверяет их перед использованием. */

#define SYS_EXIT     0   /* (s32 code) -- не возвращается */
#define SYS_WRITE    1   /* (const char *buf, u32 len) -> s32 записанных байт либо <0 */
#define SYS_READLINE 2   /* (char *buf, u32 maxlen) -> s32 длина строки (без \0) либо <0 */
#define SYS_GETCHAR  3   /* () -> s32 код символа (блокирующее чтение с клавиатуры) */
#define SYS_YIELD    4   /* () -> 0: отдать CPU планировщику (см. include/task.h) */
#define SYS_MEMINFO  5   /* (sys_meminfo_t *out) -> 0 либо <0 */
#define SYS_FORK     6   /* () -> потомку 0, родителю pid потомка, либо <0 при нехватке памяти */

/* ---- IPC (см. include/ipc.h, kernel/ipc.c за полным описанием модели) ----
 * Хендлы у SYS_PIPE_... и SYS_MQ_... -- индексы в ЛОКАЛЬНОЙ, per-process
 * таблице (см. task_t::ipc_handles, include/task.h): не переносятся между
 * процессами сами по себе, только через SYS_FORK (потомок наследует
 * таблицу целиком, с теми же номерами -- см. ipc_task_fork_copy()).
 * Хендл у SYS_SHM_... -- ГЛОБАЛЬНЫЙ id сегмента (0 .. SHM_MAX_SEGMENTS - 1),
 * действителен для любого процесса без предварительного "открытия". */
#define SYS_PIPE_CREATE  7   /* (s32 fds_out[2]) -> 0 либо <0; fds_out[0]=хендл чтения, [1]=хендл записи */
#define SYS_PIPE_READ    8   /* (s32 h, void *buf, u32 len) -> прочитано байт (0 == EOF) либо <0 */
#define SYS_PIPE_WRITE   9   /* (s32 h, const void *buf, u32 len) -> записано байт либо <0 (нет читателей) */
#define SYS_PIPE_CLOSE   10  /* (s32 h) -> 0 либо <0 */

#define SYS_MQ_CREATE    11  /* (const char *name_or_null) -> s32 хендл либо <0 */
#define SYS_MQ_SEND      12  /* (s32 h, const void *buf, u32 len) -> 0 либо <0 */
#define SYS_MQ_RECV      13  /* (s32 h, void *buf, u32 maxlen) -> длина сообщения либо <0 (буфер мал/очередь недействительна) */
#define SYS_MQ_CLOSE     14  /* (s32 h) -> 0 либо <0 */

#define SYS_SHM_CREATE   15  /* (const char *name_or_null, u32 size) -> s32 id сегмента либо <0 */
#define SYS_SHM_ATTACH   16  /* (s32 id, void **out_vaddr) -> 0 либо <0 */
#define SYS_SHM_DETACH   17  /* (void *vaddr) -> 0 либо <0; vaddr -- то, что вернул SYS_SHM_ATTACH */
#define SYS_SHM_DESTROY  18  /* (s32 id) -> 0 либо <0; аналог shmctl(IPC_RMID) */
#define SYS_EXECVE       19  /* (const char *name, u32 argc, char **argv) -> <0 on error */
#define SYS_WAITPID      20  /* (s32 pid, s32 *status_out) -> id завершившегося потомка либо <0 (нет такого потомка).
                                 pid < 0 -- любой прямой потомок SYS_FORK; pid >= 0 -- строго этот id.
                                 Блокирует вызывающего, пока подходящий потомок не завершится. status_out
                                 может быть NULL; иначе получает exit_code потомка (см. SYS_EXIT). */

/* ---- Сеть (см. include/net.h, include/tcp.h за полным описанием стека) ----
 * IPv4-адреса везде в этом ABI -- обычный u32 в ХОСТОВОМ порядке байт
 * (0xC0A80001 == 192.168.0.1), как и во внутреннем API ядра (net.h) --
 * приложению не нужно самому думать про сетевой порядок байт нигде,
 * кроме, собственно, разбора точечно-десятичной записи в текст. */
#define SYS_NET_STATUS  21  /* (sys_net_status_t *out) -> 0 либо <0 */
#define SYS_NET_DHCP    22  /* (u32 timeout_ms) -> 0 (адрес получен и уже применён) либо <0 */
#define SYS_NET_RESOLVE 23  /* (const char *host, u32 *out_ip) -> 0 либо <0 (не резолвится за разумное время) */
#define SYS_NET_PING    24  /* (u32 ip, u32 timeout_ms, u32 *out_rtt_ms) -> 0 (получен ответ) либо <0 (таймаут) */
#define SYS_TCP_CONNECT 25  /* (u32 ip, u16 port) -> s32 хендл (>=0) либо <0 */
#define SYS_TCP_SEND    26  /* (s32 h, const void *buf, u32 len) -> отправлено и подтверждено байт либо <0 */
#define SYS_TCP_RECV    27  /* (s32 h, void *buf, u32 maxlen, u32 timeout_ms) -> байт (0 == соединение закрыто удалённой стороной) либо <0 */
#define SYS_TCP_CLOSE   28  /* (s32 h) -> 0 либо <0 */

#define SYS_FS_WRITE_FILE 29 /* (const char *name, const void *data, u32 len) -> 0 либо <0.
                                 Пишет в файл ТЕКУЩЕГО активного каталога/ФС (см. include/vfs.h
                                 fs_write_file() -- RAMFS/FAT32/UFS, что бы ни было смонтировано
                                 в данный момент), перезаписывая содержимое целиком, как shell'овая
                                 команда 'write'. До этого вызова у sandboxed (ring 3, 'run')
                                 приложений вообще не было доступа к файловой системе -- нужен
                                 apps/wget.c, чтобы сохранять скачанное на диск. */

#define SYS_CONTAINER_SPAWN 30 /* (const char *manifest_path, u32 argc, char **argv) -> код
                                   возврата дочернего контейнера либо <0.
                                   Требует CAP_SPAWN (см. include/container.h) у ВЫЗЫВАЮЩЕГО --
                                   без него SYSCALL_ERR_PERM. Манифест по manifest_path (см.
                                   include/container.h за форматом, kernel/container.c за
                                   разбором) не может запросить прав больше, чем есть у самого
                                   вызывающего -- caps дочернего контейнера ДОЛЖНЫ быть
                                   подмножеством caps вызывающего, иначе тоже SYSCALL_ERR_PERM
                                   (см. sys_container_spawn(), kernel/syscall.c за точной
                                   проверкой) -- CAP_SPAWN разрешает породить контейнер, а не
                                   обойти собственные ограничения через манифест. Для CAP_IO
                                   (Фаза 4, include/container.h) действует та же логика на
                                   уровне диапазона портов: io_base/io_len ребёнка обязаны
                                   целиком лежать внутри диапазона, реально выданного самому
                                   вызывающему (а не просто наличия бита CAP_IO в caps). argv[0]
                                   ВНУТРИ дочернего контейнера всегда равен пути бинарника из
                                   манифеста (manifest.path) -- переданный сюда argv/argc это
                                   argv[1..] дочернего процесса, как в 'runc <manifest> [args...]'
                                   в shell/shell.c. Блокирует вызывающего до завершения ребёнка --
                                   как process_run_sandboxed()/container_spawn() уже делают. */

#define SYS_IRQ_WAIT 31 /* (u32 irq) -> 0 при успехе, либо <0.
                            Требует CAP_IRQ (см. include/container.h) у вызывающего, И
                            irq должен РОВНО совпадать с тем, что разрешено его манифестом
                            (task_t::irq_line, include/task.h) -- иначе SYSCALL_ERR_PERM.
                            Блокирует вызывающего до следующего срабатывания этой линии IRQ
                            (см. irqbridge_wait(), include/irqbridge.h) -- в отличие от
                            остальных "опасных" syscall'ов в этом файле, здесь сам доступ к
                            железу (порты, если есть ещё и CAP_IO) НЕ требует syscall'а вообще,
                            только ОЖИДАНИЕ события требует -- ring 3 не может просто
                            "заблокироваться, пока не придёт прерывание" сам по себе, это
                            всегда прерогатива ядра/планировщика. Первый успешный вызов для
                            данной линии на всей системе снимает с неё маску PIC и
                            устанавливает мост-обработчик (kernel/irqbridge.c) -- если линию
                            уже занял драйвер ядра (таймер -- IRQ0, сетевая карта), вызов
                            вернёт SYSCALL_ERR_PERM вместо того, чтобы отобрать её. */

/* ---- Мост кадров ядро<->драйвер-контейнер (Фаза 4, подшаг 3, см.
 * include/netbridge.h за полным описанием канала и модели
 * конкурентности). Требуют CAP_NET_BRIDGE (include/container.h) --
 * ОТДЕЛЬНОЙ capability от CAP_NET: обычному потребителю сети (curl,
 * wget, ping) эти два вызова не нужны и не должны быть доступны, это
 * привилегия ровно того контейнера, который И ЕСТЬ сетевой драйвер
 * (Фаза 4, подшаг 4, apps/rtl8139d.c -- см. CONTAINER_ARCHITECTURE.md;
 * apps/netdrv_echo.c пользуется этой парой тоже, но только для
 * демонстрации/проверки канала). */
#define SYS_NETBRIDGE_TX_DEQUEUE 32 /* (void *buf, u32 maxlen) -> длина кадра либо <0.
                                        Блокирует вызывающего, пока сетевой стек ядра не
                                        положит кадр на отправку в TX-очередь (см.
                                        netbridge_push_tx_frame(), include/netbridge.h) --
                                        это то, что драйверу-контейнеру нужно физически
                                        отправить через карту. */
#define SYS_NETBRIDGE_RX_ENQUEUE 33 /* (const void *buf, u32 len) -> 0 либо <0.
                                        Кладёт кадр, который драйвер-контейнер только что
                                        принял от карты, в RX-очередь -- заберёт его оттуда
                                        netbridge_rx_pump() (отдельный поток ядра,
                                        kernel/kernel.c) и передаст в eth_input() (net/eth.c).
                                        Неблокирующий: <0 и если len вне диапазона, и если
                                        очередь уже полна (кадр потерян, см. общий комментарий
                                        в include/netbridge.h). */

/* ---- Файловая система, сторона чтения/навигации (Фаза 5, подшаг 5.1,
 * см. CONTAINER_ARCHITECTURE.md). SYS_FS_WRITE_FILE (29, выше) был
 * первым и до этого момента единственным доступом ring-3 приложений к
 * постоянной fs -- этот подшаг добавляет обратную сторону: прочитать,
 * узнать метаданные, перейти в другой каталог. Требуют CAP_FS_READ
 * (include/container.h) -- ОТДЕЛЬНОЙ capability от CAP_FS_WRITE, по
 * тому же принципу, что CAP_NET/CAP_NET_BRIDGE выше: 'curl'/'wget'
 * (сохраняют скачанное) исторически получали только CAP_FS_WRITE и не
 * должны получить попутно ещё и право читать чужие файлы просто потому,
 * что им нужно было писать свои.
 * SYS_FS_MKDIR/SYS_FS_RM -- наоборот, ИЗМЕНЯЮТ fs, поэтому идут под уже
 * существующей CAP_FS_WRITE, а не под этой новой -- добавлены тут же,
 * заодно, раз оба уже готовы к обёртке в syscall (bool_t-сигнатура,
 * никакого нового буферного протокола не нужно), в отличие от листинга
 * каталога (fs_ls()), который печатает прямо в консоль ядра и поэтому
 * не может быть обёрнут без более глубокой переделки backend'ов
 * (RAMFS/FAT32/UFS) -- см. известное ограничение в конце этого блока. */
#define SYS_FS_READ_FILE 34 /* (const char *name, void *buf, u32 bufsize, u32 *out_size) -> 0 либо <0.
                                Читает содержимое файла ТЕКУЩЕГО активного каталога/ФС в buf
                                (максимум bufsize байт), *out_size -- реальный прочитанный
                                размер (см. fs_read_file(), include/vfs.h). Поддержано только
                                на RAMFS backend'е (как и у самого fs_read_file()) -- на
                                FAT32/UFS вернёт SYSCALL_ERR_BADH. */
#define SYS_FS_STAT      35 /* (const char *name, sys_fs_stat_t *out) -> 0 либо <0.
                                Владелец/группа/права узла (см. fs_stat(), include/vfs.h) --
                                поддержано только на RAMFS backend'е (fs_supports_permissions()),
                                на FAT32/UFS вернёт SYSCALL_ERR_BADH, у них пока нет понятия
                                владельца узла на диске. */
#define SYS_FS_PWD       36 /* (char *buf, u32 bufsize) -> 0 либо <0. Текущий каталог активной
                                ФС (см. fs_pwd(), include/vfs.h) -- ЕДИНОЕ состояние на всю
                                систему, не отдельное на процесс, см. общий комментарий у
                                CAP_FS_READ (include/container.h). */
#define SYS_FS_CD        37 /* (const char *name) -> 0 либо <0. Меняет текущий каталог активной
                                ФС (см. fs_cd(), include/vfs.h) -- тот же общий комментарий про
                                единое на всю систему состояние, что и у SYS_FS_PWD. */
#define SYS_FS_MKDIR     38 /* (const char *name) -> 0 либо <0. Требует CAP_FS_WRITE, не
                                CAP_FS_READ (см. общий комментарий выше) -- создаёт каталог в
                                текущем (см. SYS_FS_CD) каталоге активной ФС (fs_mkdir()). */
#define SYS_FS_RM        39 /* (const char *name) -> 0 либо <0. Требует CAP_FS_WRITE. Удаляет
                                файл/пустой каталог из текущего каталога активной ФС (fs_rm()). */

/* ---- Файловая система, листинг каталога (Фаза 5, подшаг 5.2, см.
 * CONTAINER_ARCHITECTURE.md). Единственный вызов подшага 5.1, который
 * не мог быть простой обёрткой существующей fs_ls() -- она печатает
 * прямо в консоль ядра через kprintf(), а не заполняет буфер (см.
 * fs_list(), include/vfs.h, и её backend-специфичные реализации
 * ramfs_list()/fat32_list()/ufs_list()). Требует ту же CAP_FS_READ,
 * что и остальные вызовы этой стороны. */
#define SYS_FS_LIST 40 /* (sys_fs_dirent_t *out, u32 max_entries, u32 *out_count) -> 0 либо <0.
                           Заполняет out[] содержимым ТЕКУЩЕГО каталога (см. SYS_FS_CD) активной
                           ФС, до max_entries штук; *out_count -- сколько записей РЕАЛЬНО есть
                           (может быть БОЛЬШЕ max_entries, если не влезло -- как обычный ls,
                           предупреждение, а не ошибка и не молчаливая обрезка). owner_uid/mode
                           осмысленны только на RAMFS backend'е -- на FAT32/UFS заполняются
                           нулями (там нет понятия владельца узла на диске). */

/* ---- Список задач (Фаза 5, подшаг 5.2). Требует НОВУЮ CAP_PS
 * (include/container.h) -- список ВСЕХ задач в системе, не только
 * своих собственных, поэтому не может быть доступен по умолчанию. */
#define SYS_PS_LIST 41 /* (sys_task_info_t *out, u32 max_entries, u32 *out_count) -> 0 либо <0.
                           Заполняет out[] списком задач planировщика (id/имя/состояние каждой,
                           см. task_count()/task_get(), include/task.h), до max_entries штук;
                           *out_count -- сколько задач РЕАЛЬНО есть (см. общий комментарий у
                           SYS_FS_LIST выше про max_entries). Сознательно НЕ отдаёт rsp/
                           stack_base/as и прочие внутренние адреса ядра -- см. sys_task_info_t. */

/* Кладёт вызывающего спать на ms миллисекунд (см. task_sleep(),
   include/task.h) -- не требует никакой capability: задача влияет
   только на саму себя, ровно как SYS_YIELD. */
#define SYS_SLEEP 42 /* (u32 ms) -> 0. Не блокирует ничего, кроме самого вызывающего. */

/* ---- Питание (Фаза 5, подшаг 5.3, см. CONTAINER_ARCHITECTURE.md).
 * Требуют CAP_POWER И ДОПОЛНИТЕЛЬНО users_current_is_root() -- см.
 * общий комментарий у CAP_POWER (include/container.h) за тем, почему
 * этого требует ядро само, а не только вызывающий контейнер. */
#define SYS_POWER_REBOOT   43 /* () -> не возвращается при успехе, <0 если отказано. */
#define SYS_POWER_SHUTDOWN 44 /* () -> не возвращается при успехе, <0 если отказано. */

/* ---- Пользователи (Фаза 5, подшаг 5.3). Требуют CAP_USERS -- см.
 * общий комментарий у CAP_USERS (include/container.h) за тем, какие
 * из них ДОПОЛНИТЕЛЬНО требуют users_current_is_root(). */
#define SYS_USERS_WHOAMI        45 /* (sys_user_info_t *out) -> 0 либо <0. Текущий вошедший
                                        пользователь -- ЕДИНОЕ состояние на всю систему, как cwd
                                        (SYS_FS_PWD) -- см. общий комментарий у CAP_USERS. */
#define SYS_USERS_LIST           46 /* (sys_user_info_t *out, u32 max_entries, u32 *out_count) -> 0
                                        либо <0. Список ВСЕХ учётных записей (без паролей,
                                        конечно) -- см. общий комментарий про max_entries у
                                        SYS_FS_LIST/SYS_PS_LIST. */
#define SYS_USERS_AUTHENTICATE   47 /* (const char *name, const char *password) -> 0, если пара
                                        верна, иначе <0. Ничего не меняет -- полезно как
                                        самостоятельная проверка (например будущему
                                        контейнеру-шеллу для реализации своего экрана входа, см.
                                        Фазу 5, подшаг 5.5). */
#define SYS_USERS_READ_PASSWORD  48 /* (char *buf, u32 maxlen) -> длина либо <0. Читает пароль с
                                        клавиатуры С МАСКИРОВКОЙ ('*') -- обёртка над
                                        users_read_password() (include/users.h). Отдельный вызов,
                                        а не флаг у SYS_READLINE: последний тут не трогаем, чтобы
                                        не менять уже существующий ABI ради одного частного
                                        случая. */
#define SYS_USERS_PASSWD         49 /* (const char *target, const char *current_pass_or_null,
                                        const char *new_pass) -> 0 либо <0. Смена пароля --
                                        РОВНО та же логика, что cmd_passwd() в shell/shell.c
                                        сегодня, теперь в ядре: если target -- сам вызывающий (по
                                        имени текущего пользователя) И текущий пользователь не
                                        root, current_pass_or_null ОБЯЗАН быть верным текущим
                                        паролем (проверяется через users_authenticate());
                                        если target -- ДРУГОЙ пользователь, требуется, чтобы
                                        текущий вошедший был root (current_pass_or_null в этом
                                        случае игнорируется). */
#define SYS_USERS_ADD            50 /* (const char *name, const char *password, bool_t is_root) ->
                                        0 либо <0. Требует ДОПОЛНИТЕЛЬНО users_current_is_root()
                                        -- см. общий комментарий у CAP_USERS. */
#define SYS_USERS_DEL            51 /* (const char *name) -> 0 либо <0. Требует ДОПОЛНИТЕЛЬНО
                                        users_current_is_root(). Сам users_del() (include/users.h)
                                        уже не позволяет удалить последнюю root-учётную запись в
                                        системе или текущего вошедшего пользователя -- эта
                                        проверка не дублируется здесь. */
#define SYS_USERS_SU             52 /* (const char *target, const char *password_or_null) -> 0
                                        либо <0. РОВНО та же логика, что cmd_su() в shell/shell.c
                                        сегодня: если текущий вошедший -- root, переключается без
                                        проверки пароля (как su в обычных Unix-подобных ОС);
                                        иначе password_or_null должен пройти
                                        users_authenticate() для target. */
#define SYS_USERS_LOGOUT         53 /* () -> 0 (после успешного нового входа). Блокирующий экран
                                        входа (users_login_prompt(), include/users.h) -- работает
                                        через клавиатуру/консоль ядра НАПРЯМУЮ, в обход обычного
                                        stdio вызывающего контейнера (тот всё равно просто ждёт,
                                        пока кто-то не введёт верную пару логин/пароль). Нужен
                                        сейчас в первую очередь для будущего контейнера-шелла
                                        (Фаза 5, подшаг 5.5) -- команда 'logout'. */

/* ---- Диски/USB/время (Фаза 5, подшаг 5.4, см. CONTAINER_ARCHITECTURE.md).
 * SYS_STORAGE_LIST/SYS_USB_LIST/SYS_USB_MOUSE_STATE требуют новую
 * CAP_SYSINFO (include/container.h) -- перечисление физического
 * "железа" машины. SYS_RTC_READ -- ИСКЛЮЧЕНИЕ, без capability (см.
 * общий комментарий у CAP_SYSINFO). */
#define SYS_STORAGE_LIST    54 /* (sys_storage_info_t *out, u32 max_entries, u32 *out_count) -> 0
                                    либо <0. Список всех известных дисков (см. storage_drive_count()/
                                    storage_get_drive(), include/storage.h) -- ТОЛЬКО перечисление
                                    (модель/размер/шина), НЕ чтение/запись секторов: сырой доступ к
                                    диску в обход файловой системы сюда сознательно не входит (это
                                    был бы способ обойти CAP_FS_READ/CAP_FS_WRITE целиком). */
#define SYS_USB_LIST         55 /* (sys_usb_info_t *out, u32 max_entries, u32 *out_count) -> 0
                                    либо <0. Список USB-устройств (см. usb_device_count()/
                                    usb_get_device(), include/usb.h). */
#define SYS_USB_MOUSE_STATE  56 /* (sys_mouse_state_t *out) -> 0 либо <0 (если мышь не
                                    подключена). См. usb_hid_mouse_present()/
                                    usb_hid_mouse_get_state() (include/usb_hid.h). */
#define SYS_RTC_READ         57 /* (sys_rtc_time_t *out) -> 0. См. rtc_read() (include/rtc.h). */

/* ---- Файловая система, монтирование/форматирование (Фаза 5, подшаг
 * 5.4). Требуют уже существующую CAP_FS_WRITE, не новую capability --
 * это структурное изменение состояния файловой системы (какой backend
 * сейчас активен), той же природы, что и SYS_FS_MKDIR/SYS_FS_RM. */
#define SYS_FS_MOUNT       58 /* (u32 drive_index) -> 0 либо <0. Монтирует диск (автоопределяя
                                  FAT32/UFS) и переключает на него активный backend -- см.
                                  fs_mount() (include/vfs.h). */
#define SYS_FS_FORMAT      59 /* (u32 drive_index, u32 fs_type) -> 0 либо <0. РАЗРУШАЕТ данные на
                                  диске -- см. fs_format()/fs_type_t (include/vfs.h). fs_type: 0=FAT32,
                                  1=UFS. */
#define SYS_FS_CHMOD       60 /* (const char *name, u32 mode) -> 0 либо <0. Права доступа --
                                  только на RAMFS backend'е (fs_supports_permissions()), как и
                                  SYS_FS_STAT -- см. fs_chmod() (include/vfs.h). */
#define SYS_FS_CHOWN       61 /* (const char *name, u32 new_uid) -> 0 либо <0. Тот же RAMFS-only
                                  комментарий, что у SYS_FS_CHMOD -- см. fs_chown() (include/vfs.h). */

/* Требует уже существующую CAP_FS_READ -- информационный вызов о самой
   файловой системе, той же природы, что SYS_FS_PWD/SYS_FS_STAT. */
#define SYS_FS_BACKEND_INFO 62 /* (sys_fs_backend_info_t *out) -> 0. Имя активного backend'а
                                   ("RAM"/"FAT32"/"UFS") и смонтирован ли вообще какой-то диск --
                                   см. fs_backend_name()/fs_is_mounted() (include/vfs.h). ВАЖНО:
                                   в этой ОС нет операции "размонтировать и вернуться на RAMFS" --
                                   ни shell/shell.c, ни fs/vfs.c её не предоставляют (см.
                                   известное ограничение подшага 5.4) -- SYS_FS_MOUNT переключает
                                   ТОЛЬКО на другой диск. */

/* Требует уже существующую CAP_NET -- та же capability, что и у
   SYS_NET_DHCP: оба -- способы настроить сетевой интерфейс. */
#define SYS_NET_SET_STATIC 63 /* (u32 ip, u32 netmask, u32 gateway, u32 dns) -> 0. См.
                                  net_set_static() (include/net.h) -- все четыре адреса в ХОСТОВОМ
                                  порядке байт (см. общий комментарий там же), а не сетевом. */

/* ---- Фаза 5, подшаг 5.5: контейнер-шелл (см. CONTAINER_ARCHITECTURE.md).
 * Два маленьких, но обязательных для полноценного интерактивного
 * шелла в ring 3 довеска, которых не хватало в 5.1-5.4: экран нельзя
 * очистить и нельзя вернуться на RAMFS без прямого вызова
 * vga_clear()/fs_use_ramfs() (обе -- функции ядра, недоступные из
 * ring 3 без syscall'а). Оба -- той же природы, что уже существующие
 * соседи (SYS_SLEEP/SYS_FS_MOUNT), поэтому под ту же логику
 * capability, что и они. */
#define SYS_CLEAR         64 /* () -> 0. Очищает текстовый экран (см. vga_clear(),
                                 include/vga.h). Не требует capability -- как
                                 SYS_SLEEP/SYS_YIELD, влияет только на общий текстовый
                                 экран, а не на состояние другого процесса или файла;
                                 то же самое мог сделать (и делает) любой обычный вывод
                                 через SYS_WRITE, который тоже ни капабилити, ни проверки
                                 не требует. */
#define SYS_FS_USE_RAMFS  65 /* () -> 0. Переключает активный backend файловой системы
                                 обратно на RAMFS (см. fs_use_ramfs(), include/vfs.h) --
                                 обратная сторона SYS_FS_MOUNT, той же природы (структурное
                                 изменение состояния ФС), поэтому под уже существующей
                                 CAP_FS_WRITE, а не новой capability. */

/* ---- Сырой блочный доступ к диску, В ОБХОД VFS (см. INSTALLER_PLAN.md
 * -- инсталлятор ОС на диск без ISO). storage_read_sectors()/
 * storage_write_sectors() (include/storage.h) уже существуют на уровне
 * ядра для драйверов IDE/AHCI/USB -- эти два syscall'а просто пробрасывают
 * их в userspace, МИМО файловой системы. Нужны затем, что MBR, таблица
 * разделов и область под GRUB core.img (сектора между MBR и первым
 * разделом) не принадлежат никакой файловой системе -- через
 * SYS_FS_WRITE_FILE их не записать в принципе, каким бы CAP_FS_WRITE
 * вызывающий ни обладал.
 *
 * Именно поэтому это САМАЯ опасная пара syscall'ов в системе -- дают
 * прямую запись в произвольный сектор произвольного физического диска,
 * включая тот, откуда сама ОС загружена. Отсюда отдельная, ни с чем не
 * объединённая capability CAP_DISK_RAW (include/container.h): ни
 * CAP_FS_WRITE (пишет файлы через VFS -- под контролем ФС), ни
 * CAP_SYSINFO (только читает список дисков, SYS_STORAGE_LIST) её не
 * подразумевают -- инсталлятору её нужно выдавать явно и осознанно
 * (см. манифесты в rootfs/containers/).
 *
 * count -- число 512-байтных секторов; за один вызов -- не больше
 * SYS_DISK_MAX_SECTORS_PER_CALL (внутри ядра большой запрос режется на
 * чанки по 255 секторов -- предел storage_ops_t::read_sectors/
 * write_sectors, см. include/storage.h, т.к. count там -- u8). */
#define SYS_DISK_MAX_SECTORS_PER_CALL 8192u /* 8192*512 = 4 MiB за один syscall, тот же порядок, что и лимит SYS_FS_WRITE_FILE/SYS_FS_READ_FILE */

#define SYS_DISK_READ_RAW  66 /* (u32 drive_index, u32 lba, u32 count, void *buf) -> 0 либо <0.
                                  Читает count секторов, начиная с lba, диска drive_index (тот же
                                  индекс, что и в SYS_STORAGE_LIST/SYS_FS_MOUNT) в buf. Требует
                                  CAP_DISK_RAW. */
#define SYS_DISK_WRITE_RAW 67 /* (u32 drive_index, u32 lba, u32 count, const void *buf) -> 0 либо
                                  <0. РАЗРУШИТЕЛЬНО: пишет count секторов из buf напрямую поверх
                                  диска drive_index, начиная с lba, без каких-либо проверок
                                  содержимого (в т.ч. можно затереть MBR/таблицу разделов/чужую
                                  файловую систему). Требует CAP_DISK_RAW. */

/* ---- Boot-ассеты (stage1.bin/stage2.bin/kernel.flat) для install-os --
 * см. INSTALLER_PLAN.md, "Самоссылающийся kernel.flat", и
 * kernel/boot_assets.h. ЭТИ ассеты сознательно НЕ идут через RAMFS: у
 * RAMFS жёсткий лимит RAMFS_FILE_DATA_SIZE = 65536 байт НА ФАЙЛ
 * (include/ramfs.h) -- kernel.flat (порядка 1+ МиБ) в него попросту не
 * влезает, а ramfs_write_file() при превышении лимита МОЛЧА обрезает
 * данные (не возвращает ошибку) -- именно так и было обнаружено это
 * ограничение: install-os записал на диск усечённые до 64 KiB байты
 * ядра при первой попытке. Вместо RAMFS -- отдельный, специально под
 * это заведённый blob прямо в .rodata самого ядра (см.
 * kernel/boot_assets_data.c, генерируется tools/gen_boot_assets.py),
 * без какого-либо лимита на размер, кроме памяти. */
#define SYS_BOOT_ASSET_READ 68 /* (u32 asset_id, void *buf, u32 bufsize, u32 *out_size) -> 0 либо
                                   <0. asset_id -- см. boot_asset_id_t (kernel/boot_assets.h).
                                   Копирует до bufsize байт ассета в buf, реальный размер -- в
                                   out_size (может быть МЕНЬШЕ bufsize -- в отличие от RAMFS,
                                   здесь при bufsize < реального размера возвращается ошибка,
                                   а не молчаливое усечение). Требует CAP_DISK_RAW -- та же
                                   capability, что и сама запись на диск, отдельную заводить
                                   незачем: у install-os (единственного потребителя) она и так
                                   уже есть. */

/* ATOMIC_PLAN.md, Фаза 3/4 -- хвост: apps/create-user.c уже пишет
 * заголовок нового пользовательского образа (userimg.h) через
 * SYS_DISK_WRITE_RAW, но ТЕЛО слота (обычный UFS поверх AES-XTS,
 * fs/ufs.c::ufs_format_encrypted(), Фаза 4) до этого syscall'а
 * инициализировать было нечем -- create-user не может напрямую
 * вызывать функции ядра. */
#define SYS_FS_FORMAT_HOME_ENCRYPTED 69 /* (u32 drive_index, u32 slot_body_lba_start,
                                   u32 slot_body_sectors, const u8 *master_key) -> 0 либо <0.
                                   Форматирует НОВЫЙ UFS поверх lib/cryptoblk.c (Фаза 4) в
                                   диапазоне [slot_body_lba_start, slot_body_lba_start+slot_body_sectors)
                                   абсолютных LBA -- см. общий комментарий у
                                   ufs_format_encrypted(), fs/ufs.c. master_key -- РОВНО 32 байта
                                   (UFS_MASTER_KEY_LEN, include/ufs.h), уже РАСШИФРОВАННЫЙ ключ
                                   (не пароль) -- ядро паролей не проверяет. РАЗРУШАЕТ всё, что
                                   было в этом диапазоне LBA до этого (та же природа, что
                                   SYS_FS_FORMAT). Требует CAP_DISK_RAW -- та же двойная защита,
                                   что и у SYS_DISK_READ_RAW/WRITE_RAW (capability И
                                   users_current_is_root(), проверяется в самом ядре -- см.
                                   общий комментарий у CAP_DISK_RAW, include/container.h): этот
                                   syscall ничем не безопаснее произвольной записи на диск,
                                   он просто удобнее для одной конкретной операции. */

#define SYS_BOOT_CONFIRM   70 /* () -> 0 либо <0. ATOMIC_PLAN.md, Фаза 6 -- см. общий
                                   комментарий в kernel/bootconfirm.c. Ядро САМО вызывает
                                   лежащую под этим syscall'ом функцию напрямую (обычным
                                   вызовом, не через int 0x80) один раз, сразу после того как
                                   дошло до рабочего состояния (успешный вход в систему,
                                   kernel/kernel.c) -- этот номер существует в первую очередь
                                   для полноты ABI (например, будущая команда шелла
                                   'confirm-boot', если понадобится подтвердить вручную раньше
                                   обычного) и для симметрии со всеми остальными
                                   диск-модифицирующими операциями (CAP_DISK_RAW +
                                   users_current_is_root(), см. общий комментарий у
                                   SYS_DISK_READ_RAW выше) -- сам по себе вызов от контейнера
                                   безвреден (переписывает boot-meta ТЕМ ЖЕ содержимым, что уже
                                   там, если он и так уже подтверждён -- см. bootmeta_mark_confirmed(),
                                   lib/bootmeta.c), просто не имеет смысла вне контекста самого
                                   ядра. Возвращает <0, если загрузка была НЕ через
                                   boot/stage2_atomic.s (atomic_boot_valid=FALSE, include/bootinfo.h)
                                   -- НЕ ошибка в смысле "что-то сломалось", а "здесь нечего
                                   подтверждать" (тот же принцип, что и у остальных "не
                                   применимо" в этом ABI). */

/* ATOMIC_PLAN.md, Фаза 7 -- update-os. ПОЧЕМУ эти два syscall'а вообще
 * понадобились, а не переиспользован SYS_BOOT_ASSET_READ +
 * SYS_DISK_WRITE_RAW из userspace, как делает install-os для
 * stage1/stage2 (маленьких ассетов): kernel.flat давно перерос
 * APP_MAX_SIZE (2 MiB, include/loader.h, ЖЁСТКО привязан к разметке
 * PDE процесса, include/memlayout.h -- поднимать его ради одного этого
 * буфера означало бы менять адресную раскладку ВСЕХ процессов) -- ни
 * один userspace-буфер физически не может вместить его целиком.
 * Смысла копировать байты asset'а в userspace и обратно всё равно не
 * было -- ядро и так держит их в собственном .rodata (см.
 * kernel/boot_assets_data.c) -- эти два syscall'а просто делают то,
 * что раньше делали install_region()/сверка в userspace, но НАПРЯМУЮ
 * между .rodata ядра и диском, чанками, никогда не требуя буфера
 * размером с весь ассет ни в ядре, ни тем более в приложении. */
#define SYS_BOOT_ASSET_WRITE_TO_DISK 71 /* (u32 asset_id, u32 drive_index, u32 lba_start,
                                   u32 region_sectors) -> >=0 (реальный размер ассета в
                                   байтах) либо <0. Требует CAP_DISK_RAW (та же capability,
                                   что и сама запись на диск) -- НЕ требует root отдельно
                                   (в отличие от SYS_FS_FORMAT_HOME_ENCRYPTED и т.п.) --
                                   см. общий комментарий у CAP_DISK_RAW, include/container.h:
                                   у update-os и так нет иного способа получить disk_raw в
                                   манифесте, кроме как через тот же принцип. Пишет ассет
                                   asset_id (kernel/boot_assets_data.c) на диск drive_index
                                   начиная с lba_start, ЧАНКАМИ (128 секторов = 64 КиБ за
                                   раз, см. kernel/syscall.c) НАПРЯМУЮ из .rodata ядра --
                                   ни один буфер размером с весь ассет нигде не заводится.
                                   Реальный размер ассета ДОЛЖЕН уместиться в
                                   region_sectors*512 байт -- иначе SYSCALL_ERR_TOOBIG, ни
                                   байта на диск не уходит. Остаток region_sectors ПОСЛЕ
                                   реального размера ассета зануляется (тот же принцип, что
                                   write_zero_padding(), apps/install-os.c, только теперь
                                   кернел-side) -- старое содержимое диска за пределами
                                   нового kernel.flat не должно остаться читаемым мусором.
                                   НЕ трогает boot-meta -- переключение активного слота
                                   (ATOMIC_PLAN.md, Фаза 7) остаётся ЦЕЛИКОМ на совести
                                   вызывающего кода (update-os), ПОСЛЕ успешной проверки
                                   через SYS_BOOT_ASSET_VERIFY_ON_DISK ниже -- этот syscall
                                   сам по себе НИЧЕГО не подтверждает и не гарантирует
                                   целостность записанного (см. общий комментарий там же за
                                   тем, почему верификация -- отдельный, обязательный шаг). */
#define SYS_BOOT_ASSET_VERIFY_ON_DISK 72 /* (u32 asset_id, u32 drive_index, u32 lba_start)
                                   -> 0 (совпало) либо <0. Требует CAP_DISK_RAW, та же
                                   логика, что у SYS_BOOT_ASSET_WRITE_TO_DISK выше. Считает
                                   CRC32 (lib/crc32.c, тот же алгоритм, что boot-meta/GPT)
                                   исходного ассета ЦЕЛИКОМ ЗА ОДИН РАЗ (он уже в .rodata
                                   ядра, копировать/чанковать незачем) и CRC32 того, что
                                   реально лежит на диске начиная с lba_start (столько
                                   секторов, сколько нужно под реальный размер ассета,
                                   ЧАНКАМИ -- см. общий комментарий у
                                   SYS_BOOT_ASSET_WRITE_TO_DISK -- накапливая через
                                   crc32(initial, ...), см. её общий комментарий,
                                   include/crc32.h, за тем, почему это математически то же
                                   самое, что посчитать по всем данным сразу). Несовпадение
                                   -- ОДНА ошибка (SYSCALL_ERR_BADH) что на реальный
                                   бит-флип на диске, что на ошибку чтения посередине -- тот
                                   же принцип "не совпало -- значит не доверяем", что у
                                   bootmeta_parse()/gpt_parse() (см. их комментарии) --
                                   вызывающему коду (update-os) в любом из этих случаев
                                   ОДИНАКОВО нельзя переключать boot-meta на этот слот. */

typedef struct {
    bool_t up;          /* сетевая карта найдена и инициализирована */
    bool_t dhcp_bound;  /* TRUE, если ip/netmask/gateway/dns получены по DHCP, а не заданы вручную */
    u32 ip;
    u32 netmask;
    u32 gateway;
    u32 dns;
    u8  mac[6];
} sys_net_status_t;

typedef struct {
    u32 mem_total_bytes;
    u32 mem_free_bytes;
    u32 heap_total_bytes;
    u32 heap_used_bytes;
} sys_meminfo_t;

typedef struct {
    u32 owner_uid;
    u32 owner_gid;
    u32 mode; /* младшие 9 бит: rwxrwxrwx, см. RAMFS_PERM_* (include/ramfs.h) */
} sys_fs_stat_t; /* == ramfs_perm_info_t (include/ramfs.h) поле-в-поле, но
    отдельный тип: этот файл -- граница ABI ядро<->userspace, а
    ramfs_perm_info_t -- внутренний тип конкретного backend'а VFS (см.
    fs_stat(), include/vfs.h), их совпадение сегодня -- не гарантия на
    будущее (например, если у FAT32/UFS появится своё понятие владельца
    с другим набором полей). */

/* Известное ограничение этого подшага (Фаза 5.1): листинг каталога
   (fs_ls()/ramfs_ls()/fat32_ls()/ufs_ls(), include/vfs.h) печатает
   прямо в консоль ядра через kprintf() -- то, что годится для
   shell/shell.c (пока она сама линкуется в ядро, см. CONTAINER_ARCHITECTURE.md,
   Фаза 5), совершенно не годится для ring-3 контейнера: ему нужен
   СТРУКТУРИРОВАННЫЙ список в СВОЙ буфер, который он потом сам решит,
   как печатать (или не печатать вовсе). Снято подшагом 5.2 -- см.
   sys_fs_dirent_t/SYS_FS_LIST ниже.*/

#define SYS_FS_DIRENT_NAME_MAX 32
typedef struct {
    char name[SYS_FS_DIRENT_NAME_MAX];
    u32  size;
    u32  owner_uid;
    u32  mode;
    bool_t is_dir;
    bool_t is_app;
} sys_fs_dirent_t; /* == fs_dirent_t (include/ramfs.h) поле-в-поле, тот же
    принцип разделения ABI и внутреннего представления, что и у
    sys_fs_stat_t/ramfs_perm_info_t (см. там же). */

#define SYS_TASK_NAME_MAX 32
typedef struct {
    u32  id;
    char name[SYS_TASK_NAME_MAX];
    u32  state; /* 0=ready,1=running,2=blocked,3=terminated -- см. task_state_t (include/task.h),
                    тот же порядок значений, но отдельный тип: см. общий принцип у sys_fs_stat_t. */
} sys_task_info_t;

#define SYS_USER_NAME_MAX 32
typedef struct {
    u32  uid;
    char name[SYS_USER_NAME_MAX];
    bool_t is_root;
} sys_user_info_t; /* используется и для SYS_USERS_WHOAMI (один элемент), и
    для SYS_USERS_LIST (массив) -- см. user_t (include/users.h), тот же
    принцип разделения ABI и внутреннего представления, что и у
    sys_fs_stat_t: НИКОГДА не включает пароль, даже хэшированный. */

typedef struct {
    bool_t present;
    u32  bus; /* 0=IDE,1=SATA,2=USB -- см. storage_bus_t (include/storage.h), тот же порядок значений */
    u32  sectors; /* по 512 байт каждый */
    char model[41];
    char location[24];
} sys_storage_info_t; /* == storage_dev_t (include/storage.h) без ops/drv_ctx
    (внутренние указатели ядра, отдавать их наружу незачем и небезопасно)
    -- тот же принцип, что и у остальных sys_*_info_t в этом файле. */

typedef struct {
    bool_t present;
    u32  speed; /* 0=low(1.5Mbit),1=full(12Mbit) -- см. usb_speed_t (include/usb.h) */
    u16  vendor_id, product_id;
    u8   dev_class, dev_subclass, dev_protocol;
} sys_usb_info_t; /* == usb_device_t (include/usb.h) без портов/endpoint'ов
    (детали шины, не нужны для простого перечисления "что подключено") */

typedef struct {
    bool_t present; /* FALSE, если мышь не подключена -- x/y/buttons тогда не осмысленны */
    u8   buttons;   /* битовая маска, см. usb_hid_mouse_get_state() (include/usb_hid.h) */
    s32  x, y;
} sys_mouse_state_t;

typedef struct {
    u8  second, minute, hour;
    u8  day, month;
    u16 year;
} sys_rtc_time_t; /* == rtc_time_t (include/rtc.h) поле-в-поле, тот же
    принцип разделения ABI и внутреннего представления, что и везде в
    этом файле. */

typedef struct {
    char name[16]; /* "RAM", "FAT32" или "UFS" -- см. fs_backend_name() (include/vfs.h) */
    bool_t is_mounted;
} sys_fs_backend_info_t;

#define SYSCALL_ERR_FAULT  (-1)   /* указатель вне разрешённой памяти программы */
#define SYSCALL_ERR_TOOBIG (-2)   /* превышен допустимый размер аргумента */
#define SYSCALL_ERR_NOMEM  (-3)   /* нехватка памяти ядра (kheap/pmm) под создаваемый объект */
#define SYSCALL_ERR_BADH   (-4)   /* недействительный IPC-хендл/id */
#define SYSCALL_ERR_PERM   (-5)   /* запрещено манифестом контейнера (см. include/container.h) */
#define SYSCALL_ERR_NOSYS  (-38)  /* неизвестный номер вызова (как errno ENOSYS) */

#endif

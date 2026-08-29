/* syscall.c -- диспетчер `int 0x80`. См. include/syscall.h и
 * include/sysabi.h за общей идеей и номерами. */

#include "syscall.h"
#include "sysabi.h"
#include "idt.h"
#include "gdt.h"
#include "uaccess.h"
#include "process.h"
#include "task.h"
#include "container.h"
#include "irqbridge.h"
#include "netbridge.h"
#include "kprintf.h"
#include "keyboard.h"
#include "console.h"
#include "vga.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "ipc.h"
#include "loader.h"
#include "vfs.h"
#include "users.h"
#include "power.h"
#include "storage.h"
#include "boot_assets.h"
#include "bootconfirm.h"
#include "crc32.h"
#include "usb.h"
#include "usb_hid.h"
#include "rtc.h"
#include "net.h"
#include "dns.h"
#include "dhcp.h"
#include "icmp.h"
#include "tcp.h"

void syscall_init(void) {
    idt_set_gate(0x80, (u64)isr128, GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT_USER);
}

static s64 sys_write(const char *buf, u32 len) {
    if (len > 4096) len = 4096;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;
    for (u32 i = 0; i < len; i++) vga_putc(buf[i]);
    return (s64)len;
}

static s64 sys_readline(char *buf, u32 maxlen) {
    if (maxlen == 0 || maxlen > 4096) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, maxlen)) return SYSCALL_ERR_FAULT;
    return (s64)console_readline(buf, maxlen);
}

static s64 sys_meminfo(sys_meminfo_t *out) {
    if (!uaccess_check(out, (u32)sizeof(*out))) return SYSCALL_ERR_FAULT;

    sys_meminfo_t info;
    info.mem_total_bytes  = pmm_total_memory_bytes();
    info.mem_free_bytes   = pmm_free_memory_bytes();
    info.heap_total_bytes = kheap_total_bytes();
    info.heap_used_bytes  = kheap_used_bytes();

    memcpy(out, &info, sizeof(info));
    return 0;
}

/* Копирует имя IPC-объекта (SYS_MQ_CREATE/SYS_SHM_CREATE -- пайпы имён
   не имеют) из пользовательской памяти в буфер ядра размером max байт,
   с гарантированным завершающим нулём. В отличие от обычного strcpy() из
   пользовательского указателя, НЕ сканирует память неограниченной длины
   в поисках '\0' -- сначала проверяет через uaccess_check() ровно max
   байт как единый диапазон (тот же принцип, что и во всех остальных
   syscall'ах этого файла: длина всегда известна заранее, а не
   выясняется чтением из недоверенной памяти), и только потом ищет
   ноль-терминатор уже в СВОЕЙ, доверенной копии. */
static bool_t copy_user_name(const char *uname, char *kname, u32 max) {
    if (!uaccess_check(uname, max)) return FALSE;
    u32 i;
    for (i = 0; i < max - 1 && uname[i] != '\0'; i++) {
        kname[i] = uname[i];
    }
    kname[i] = '\0';
    return TRUE;
}

/* Копирует argv[] -- argc указателей на NUL-терминированные строки в
   пользовательском адресном пространстве ТЕКУЩЕЙ задачи -- в argc
   указателей на СВОИ, отдельно выделенные в куче ядра копии этих строк.
   Раньше это было развёрнуто прямо внутри sys_execve(); теперь тот же
   код нужен и sys_container_spawn() ниже, так что он вынесен сюда без
   каких-либо изменений в логике/проверках (те же лимиты: argc <= 128,
   каждая строка < 256 байт -- см. комментарии на местах прежних
   проверок, они просто переехали).

   При успехе (возврат 0) кладёт в *out_kargv свежий массив (или 0, если
   argc == 0) -- вызывающий обязан освободить его через free_kargv()
   ниже. При ошибке возвращает SYSCALL_ERR_* < 0 и *out_kargv не трогает
   (ничего частично выделенного наружу не просачивается -- сама функция
   подчищает за собой при любой ошибке на середине цикла). */
static s64 copy_user_argv(u32 argc, char **argv, char ***out_kargv) {
    if (argc > 128) return SYSCALL_ERR_TOOBIG;

    if (argc > 0 && !uaccess_check(argv, (u32)(argc * sizeof(char *)))) {
        return SYSCALL_ERR_FAULT;
    }

    char **kargv = NULL;
    if (argc > 0) {
        kargv = kmalloc((u32)(argc * sizeof(char *)));
        if (!kargv) return SYSCALL_ERR_NOMEM;
    }

    for (u32 i = 0; i < argc; i++) {
        char *uarg;
        if (!uaccess_check(&argv[i], (u32)sizeof(char *))) {
            for (u32 j = 0; j < i; j++) kfree(kargv[j]);
            kfree(kargv);
            return SYSCALL_ERR_FAULT;
        }
        memcpy(&uarg, &argv[i], sizeof(char *));

        u32 len = 0;
        while (TRUE) {
            if (!uaccess_check(uarg + len, 1)) {
                for (u32 j = 0; j < i; j++) kfree(kargv[j]);
                kfree(kargv);
                return SYSCALL_ERR_FAULT;
            }
            if (uarg[len] == '\0') break;
            len++;
            if (len >= 256) {
                for (u32 j = 0; j < i; j++) kfree(kargv[j]);
                kfree(kargv);
                return SYSCALL_ERR_TOOBIG;
            }
        }

        kargv[i] = kmalloc(len + 1);
        if (!kargv[i]) {
            for (u32 j = 0; j < i; j++) kfree(kargv[j]);
            kfree(kargv);
            return SYSCALL_ERR_NOMEM;
        }
        memcpy(kargv[i], uarg, len + 1);
    }

    *out_kargv = kargv;
    return 0;
}

/* Освобождает то, что вернул copy_user_argv() выше -- каждую строку и
   сам массив. argc здесь ДОЛЖЕН быть тем же самым argc, что был передан
   copy_user_argv() (сколько элементов реально выделено). */
static void free_kargv(u32 argc, char **kargv) {
    for (u32 i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
}

/* ---- пайпы (см. include/ipc.h) ---- */

static s64 sys_pipe_create(s32 *fds_out) {
    if (!uaccess_check(fds_out, 2 * (u32)sizeof(s32))) return SYSCALL_ERR_FAULT;

    pipe_t *p;
    if (!pipe_create(&p)) return SYSCALL_ERR_NOMEM;

    task_t *t = task_current();
    s32 rh = ipc_handle_alloc(t, IPC_HANDLE_PIPE_READ, p, 0);
    if (rh < 0) {
        pipe_close_read(p);
        pipe_close_write(p);
        return SYSCALL_ERR_NOMEM;
    }
    s32 wh = ipc_handle_alloc(t, IPC_HANDLE_PIPE_WRITE, p, 0);
    if (wh < 0) {
        ipc_handle_close(t, rh); /* закрывает и читающий конец, и слот rh */
        pipe_close_write(p);
        return SYSCALL_ERR_NOMEM;
    }

    s32 fds[2] = { rh, wh };
    memcpy(fds_out, fds, sizeof(fds));
    return 0;
}

static s64 sys_pipe_read(s32 h, void *buf, u32 len) {
    if (len > 4096) len = 4096;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;
    pipe_t *p = (pipe_t *)ipc_handle_get(task_current(), h, IPC_HANDLE_PIPE_READ);
    if (!p) return SYSCALL_ERR_BADH;
    return pipe_read(p, buf, len);
}

static s64 sys_pipe_write(s32 h, const void *buf, u32 len) {
    if (len > 4096) len = 4096;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;
    pipe_t *p = (pipe_t *)ipc_handle_get(task_current(), h, IPC_HANDLE_PIPE_WRITE);
    if (!p) return SYSCALL_ERR_BADH;
    return pipe_write(p, buf, len);
}

static s64 sys_pipe_close(s32 h) {
    return ipc_handle_close(task_current(), h) ? 0 : SYSCALL_ERR_BADH;
}

/* ---- очереди сообщений (см. include/ipc.h) ---- */

static s64 sys_mq_create(const char *name) {
    char kname[MQ_NAME_MAX];
    const char *use_name = 0;
    if (name) {
        if (!copy_user_name(name, kname, MQ_NAME_MAX)) return SYSCALL_ERR_FAULT;
        use_name = kname;
    }

    mqueue_t *q = mq_open_or_create(use_name);
    if (!q) return SYSCALL_ERR_NOMEM;

    s32 h = ipc_handle_alloc(task_current(), IPC_HANDLE_MQUEUE, q, 0);
    if (h < 0) {
        mq_close(q);
        return SYSCALL_ERR_NOMEM;
    }
    return h;
}

static s64 sys_mq_send(s32 h, const void *buf, u32 len) {
    if (len > MQ_MAX_MSG_SIZE) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;
    mqueue_t *q = (mqueue_t *)ipc_handle_get(task_current(), h, IPC_HANDLE_MQUEUE);
    if (!q) return SYSCALL_ERR_BADH;
    return mq_send(q, buf, len);
}

static s64 sys_mq_recv(s32 h, void *buf, u32 maxlen) {
    if (maxlen > MQ_MAX_MSG_SIZE) maxlen = MQ_MAX_MSG_SIZE;
    if (!uaccess_check(buf, maxlen)) return SYSCALL_ERR_FAULT;
    mqueue_t *q = (mqueue_t *)ipc_handle_get(task_current(), h, IPC_HANDLE_MQUEUE);
    if (!q) return SYSCALL_ERR_BADH;
    return mq_recv(q, buf, maxlen);
}

static s64 sys_mq_close(s32 h) {
    return ipc_handle_close(task_current(), h) ? 0 : SYSCALL_ERR_BADH;
}

/* ---- разделяемая память (см. include/ipc.h) ---- */

static s64 sys_shm_create(const char *name, u32 size) {
    if (!(task_current()->caps & CAP_SHM)) return SYSCALL_ERR_PERM;

    char kname[SHM_NAME_MAX];
    const char *use_name = 0;
    if (name) {
        if (!copy_user_name(name, kname, SHM_NAME_MAX)) return SYSCALL_ERR_FAULT;
        use_name = kname;
    }

    s32 id = shm_open_or_create(use_name, size);
    if (id == -1) return SYSCALL_ERR_TOOBIG; /* size вне диапазона либо нет свободных слотов */
    if (id == -2) return SYSCALL_ERR_NOMEM;  /* нехватка физической памяти под кадры */
    return id;
}

static s64 sys_shm_attach(s32 id, void **out_vaddr) {
    if (!(task_current()->caps & CAP_SHM)) return SYSCALL_ERR_PERM;
    if (!uaccess_check(out_vaddr, (u32)sizeof(void *))) return SYSCALL_ERR_FAULT;

    shm_seg_t *seg = shm_get(id);
    if (!seg) return SYSCALL_ERR_BADH;

    u64 vaddr;
    if (!shm_attach(seg, task_current(), &vaddr)) return SYSCALL_ERR_NOMEM;

    void *v = (void *)(uintptr_t)vaddr;
    memcpy(out_vaddr, &v, sizeof(v));
    return 0;
}

static s64 sys_shm_detach(void *vaddr) {
    return shm_detach_by_vaddr(task_current(), (u64)(uintptr_t)vaddr) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_shm_destroy(s32 id) {
    if (!(task_current()->caps & CAP_SHM)) return SYSCALL_ERR_PERM;
    return shm_mark_destroy(id) ? 0 : SYSCALL_ERR_BADH;
}

/* status_out может быть NULL (программе не интересен код возврата) --
   в этом случае просто не трогаем пользовательскую память вовсе; иначе
   проверяем диапазон ДО долгого блокирующего ожидания в
   process_waitpid(), чтобы не тратить его впустую на заведомо неверный
   указатель. */
static s64 sys_waitpid(s32 pid, s32 *status_out) {
    if (status_out && !uaccess_check(status_out, (u32)sizeof(s32))) return SYSCALL_ERR_FAULT;

    s32 code = 0;
    s64 res = process_waitpid(pid, &code);
    if (res >= 0 && status_out) {
        memcpy(status_out, &code, sizeof(code));
    }
    return res;
}

static s64 sys_execve(const char *name, u32 argc, char **argv, registers_t *regs) {
    char kname[128];
    if (!copy_user_name(name, kname, sizeof(kname))) {
        return SYSCALL_ERR_FAULT;
    }

    char **kargv = NULL;
    s64 aerr = copy_user_argv(argc, argv, &kargv);
    if (aerr < 0) return aerr;

    /* Проверяем, что файл существует и является исполняемым */
    if (fs_get_app(kname) != RAMFS_APP_BINARY) {
        free_kargv(argc, kargv);
        return SYSCALL_ERR_FAULT; /* нет такого бинарника */
    }

    /* Читаем ELF-файл целиком в буфер ядра */
    u32 size = 0;
    void *buf = kmalloc(RAMFS_FILE_DATA_SIZE);
    if (!buf) {
        free_kargv(argc, kargv);
        return SYSCALL_ERR_NOMEM;
    }

    if (!fs_read_file(kname, buf, RAMFS_FILE_DATA_SIZE, &size) || size == 0) {
        free_kargv(argc, kargv);
        kfree(buf);
        return SYSCALL_ERR_FAULT;
    }

    /* Обновляем имя задачи ДО process_execve -- после неё старый AS
       уничтожен, но kname[] лежит на стеке ядра и доступен всегда. */
    {
        task_t *cur = task_current();
        const char *leaf = kname;
        for (u32 i = 0; kname[i] != '\0'; i++) {
            if (kname[i] == '/') leaf = &kname[i + 1];
        }
        strncpy(cur->name, leaf, TASK_NAME_MAX - 1);
        cur->name[TASK_NAME_MAX - 1] = '\0';
    }

    /* Заменяем образ процесса. При успехе regs модифицируется так, что
       после iretq в ISR мы окажемся в точке входа нового ELF в ring 3.
       При ошибке -- возвращаем ошибку; старый образ ещё цел. */
    s64 res = process_execve(buf, size, argc, kargv, regs);

    /* buf и kargv[] -- в куче ядра; доступны и после смены AS. */
    free_kargv(argc, kargv);
    kfree(buf);

    return (res < 0) ? SYSCALL_ERR_FAULT : 0;
}


/* ---- сеть (см. include/sysabi.h SYS_NET_.../SYS_TCP_..., include/net.h,
   include/tcp.h за общим описанием стека) ---- */

static s64 sys_net_status(sys_net_status_t *out) {
    if (!uaccess_check(out, (u32)sizeof(*out))) return SYSCALL_ERR_FAULT;

    net_config_t cfg;
    net_get_config(&cfg);

    sys_net_status_t st;
    st.up = cfg.up;
    st.dhcp_bound = cfg.dhcp_bound;
    st.ip = cfg.ip;
    st.netmask = cfg.netmask;
    st.gateway = cfg.gateway;
    st.dns = cfg.dns;
    memcpy(st.mac, cfg.mac.b, 6);

    memcpy(out, &st, sizeof(st));
    return 0;
}

static s64 sys_net_dhcp(u32 timeout_ms) {
    if (!(task_current()->caps & CAP_NET)) return SYSCALL_ERR_PERM;
    if (timeout_ms == 0 || timeout_ms > 30000) timeout_ms = 8000;
    return dhcp_request(timeout_ms) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_net_resolve(const char *host, u32 *out_ip) {
    if (!(task_current()->caps & CAP_NET)) return SYSCALL_ERR_PERM;
    char khost[256];
    if (!copy_user_name(host, khost, sizeof(khost))) return SYSCALL_ERR_FAULT;
    if (!uaccess_check(out_ip, (u32)sizeof(*out_ip))) return SYSCALL_ERR_FAULT;

    ip_addr_t ip;
    if (!dns_resolve(khost, 5000, &ip)) return SYSCALL_ERR_BADH;

    memcpy(out_ip, &ip, sizeof(ip));
    return 0;
}

static s64 sys_net_ping(u32 ip, u32 timeout_ms, u32 *out_rtt_ms) {
    if (!(task_current()->caps & CAP_NET)) return SYSCALL_ERR_PERM;
    if (out_rtt_ms && !uaccess_check(out_rtt_ms, (u32)sizeof(*out_rtt_ms))) return SYSCALL_ERR_FAULT;
    if (timeout_ms == 0 || timeout_ms > 30000) timeout_ms = 2000;

    static u16 g_seq = 0;
    u16 id = (u16)(task_current()->id & 0xFFFFu);
    u32 rtt;
    if (!icmp_ping((ip_addr_t)ip, id, g_seq++, timeout_ms, &rtt)) return SYSCALL_ERR_BADH;

    if (out_rtt_ms) memcpy(out_rtt_ms, &rtt, sizeof(rtt));
    return 0;
}

static s64 sys_tcp_connect(u32 ip, u16 port) {
    if (!(task_current()->caps & CAP_NET)) return SYSCALL_ERR_PERM;
    s32 sock = tcp_connect((ip_addr_t)ip, port, 5000);
    if (sock < 0) return SYSCALL_ERR_BADH;

    /* +1: ipc_handle_alloc() хранит obj как void*, а ipc_handle_get()
       ниже (в sys_tcp_send/sys_tcp_recv) не может отличить "хендл не
       найден" (возвращает 0) от "хендл найден, а сохранённый obj -- это
       ЛЕГИТИМНЫЙ sock == 0" (тоже 0!) -- tcp.c нумерует сокеты с нуля,
       так что sock == 0 -- совершенно обычное, скорее всего самое частое
       значение (первое же TCP-соединение в системе). Без +1 здесь любой
       tcp_send()/tcp_recv() на такое соединение ошибочно проваливался бы
       с SYSCALL_ERR_BADH, хотя сокет полностью исправен. */
    s32 h = ipc_handle_alloc(task_current(), IPC_HANDLE_TCP_SOCKET,
                              (void *)(intptr_t)(sock + 1), 0);
    if (h < 0) {
        tcp_close(sock);
        return SYSCALL_ERR_NOMEM;
    }
    return h;
}

static s64 sys_tcp_send(s32 h, const void *buf, u32 len) {
    if (len > 65536) len = 65536;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;
    void *obj = ipc_handle_get(task_current(), h, IPC_HANDLE_TCP_SOCKET);
    if (!obj) return SYSCALL_ERR_BADH;
    return tcp_send((s32)(intptr_t)obj - 1, buf, len, 10000);
}

static s64 sys_tcp_recv(s32 h, void *buf, u32 maxlen, u32 timeout_ms) {
    if (maxlen > 65536) maxlen = 65536;
    if (!uaccess_check(buf, maxlen)) return SYSCALL_ERR_FAULT;
    if (timeout_ms == 0 || timeout_ms > 60000) timeout_ms = 5000;
    void *obj = ipc_handle_get(task_current(), h, IPC_HANDLE_TCP_SOCKET);
    if (!obj) return SYSCALL_ERR_BADH;
    return tcp_recv((s32)(intptr_t)obj - 1, buf, maxlen, timeout_ms);
}

static s64 sys_tcp_close(s32 h) {
    return ipc_handle_close(task_current(), h) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_write_file(const char *name, const void *data, u32 len) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;
    if (len > 4 * 1024 * 1024u) return SYSCALL_ERR_TOOBIG; /* с запасом хватает и для больших страниц/файлов */
    if (!uaccess_check(data, len)) return SYSCALL_ERR_FAULT;

    return fs_write_file(kname, (const char *)data, len) ? 0 : SYSCALL_ERR_BADH;
}

/* ---- Файловая система, сторона чтения/навигации (Фаза 5, подшаг 5.1,
   см. include/sysabi.h за общим описанием этого блока). ---- */

static s64 sys_fs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;
    if (bufsize > 4 * 1024 * 1024u) bufsize = 4 * 1024 * 1024u;
    if (!uaccess_check(buf, bufsize)) return SYSCALL_ERR_FAULT;
    if (out_size && !uaccess_check(out_size, sizeof(u32))) return SYSCALL_ERR_FAULT;

    u32 size = 0;
    if (!fs_read_file(kname, buf, bufsize, &size)) return SYSCALL_ERR_BADH;
    if (out_size) *out_size = size;
    return 0;
}

static s64 sys_fs_stat(const char *name, sys_fs_stat_t *out) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;
    if (!uaccess_check(out, sizeof(*out))) return SYSCALL_ERR_FAULT;

    /* ramfs_perm_info_t совпадает с sys_fs_stat_t поле-в-поле (см.
       комментарий у sys_fs_stat_t, include/sysabi.h) -- копия через
       локальную переменную, а не приведение указателя, чтобы граница
       ABI и внутренний тип VFS оставались НЕЗАВИСИМО меняемыми. */
    ramfs_perm_info_t info;
    if (!fs_stat(kname, &info)) return SYSCALL_ERR_BADH;

    sys_fs_stat_t kout;
    kout.owner_uid = info.owner_uid;
    kout.owner_gid = info.owner_gid;
    kout.mode = info.mode;
    memcpy(out, &kout, sizeof(kout));
    return 0;
}

static s64 sys_fs_pwd(char *buf, u32 bufsize) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    if (bufsize == 0 || bufsize > 4096) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, bufsize)) return SYSCALL_ERR_FAULT;

    char kbuf[256];
    u32 kbufsize = bufsize < sizeof(kbuf) ? bufsize : sizeof(kbuf);
    fs_pwd(kbuf, kbufsize);
    memcpy(buf, kbuf, kbufsize);
    return 0;
}

static s64 sys_fs_cd(const char *name) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return fs_cd(kname) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_mkdir(const char *name) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return fs_mkdir(kname) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_rm(const char *name) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return fs_rm(kname) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_list(sys_fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    if (max_entries > 256) max_entries = 256; /* с запасом хватает любому разумному каталогу; не даёт запросить гигантский kmalloc ниже по чужому счёту */
    if (max_entries > 0 && !uaccess_check(out, max_entries * (u32)sizeof(sys_fs_dirent_t))) return SYSCALL_ERR_FAULT;
    if (out_count && !uaccess_check(out_count, sizeof(u32))) return SYSCALL_ERR_FAULT;

    /* fs_dirent_t совпадает с sys_fs_dirent_t поле-в-поле (см. комментарий
       у sys_fs_dirent_t, include/sysabi.h) -- собираем в кучу ядра, а не
       приводим указатель на пользовательский буфер напрямую, по тем же
       причинам, что и sys_fs_stat() выше: граница ABI и внутренний тип
       VFS должны оставаться независимо меняемыми. */
    fs_dirent_t kbuf[256];
    u32 count = 0;
    if (!fs_list(kbuf, max_entries, &count)) return SYSCALL_ERR_BADH;

    u32 to_copy = count < max_entries ? count : max_entries;
    for (u32 i = 0; i < to_copy; i++) {
        sys_fs_dirent_t d;
        memset(&d, 0, sizeof(d));
        strncpy(d.name, kbuf[i].name, SYS_FS_DIRENT_NAME_MAX - 1);
        d.size = kbuf[i].size;
        d.owner_uid = kbuf[i].owner_uid;
        d.mode = kbuf[i].mode;
        d.is_dir = kbuf[i].is_dir;
        d.is_app = kbuf[i].is_app;
        memcpy(&out[i], &d, sizeof(d));
    }
    if (out_count) *out_count = count;
    return 0;
}

static s64 sys_ps_list(sys_task_info_t *out, u32 max_entries, u32 *out_count) {
    if (!(task_current()->caps & CAP_PS)) return SYSCALL_ERR_PERM;
    if (max_entries > 256) max_entries = 256; /* см. sys_fs_list() выше -- то же самое соображение про размер kbuf */
    if (max_entries > 0 && !uaccess_check(out, max_entries * (u32)sizeof(sys_task_info_t))) return SYSCALL_ERR_FAULT;
    if (out_count && !uaccess_check(out_count, sizeof(u32))) return SYSCALL_ERR_FAULT;

    u32 total = task_count();
    u32 to_copy = total < max_entries ? total : max_entries;
    for (u32 i = 0; i < to_copy; i++) {
        task_t *t = task_get(i);
        if (!t) break; /* список мог измениться между task_count() и этим task_get(), см. их общий комментарий (include/task.h) -- не ошибка, просто отдаём меньше */

        sys_task_info_t info;
        memset(&info, 0, sizeof(info));
        info.id = t->id;
        strncpy(info.name, t->name, SYS_TASK_NAME_MAX - 1);
        info.state = (u32)t->state;
        memcpy(&out[i], &info, sizeof(info));
    }
    if (out_count) *out_count = total;
    return 0;
}

static s64 sys_sleep(u32 ms) {
    task_sleep(ms);
    return 0;
}

/* ---- Питание (Фаза 5, подшаг 5.3, см. include/sysabi.h за общим
   описанием этого блока). ---- */

static s64 sys_power_reboot(void) {
    if (!(task_current()->caps & CAP_POWER)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM;
    power_reboot();
    return 0; /* не достигается при успехе */
}

static s64 sys_power_shutdown(void) {
    if (!(task_current()->caps & CAP_POWER)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM;
    power_shutdown();
    return 0; /* не достигается при успехе */
}

/* ---- Пользователи (Фаза 5, подшаг 5.3). ---- */

static void fill_user_info(sys_user_info_t *out, u32 uid, const char *name, bool_t is_root) {
    memset(out, 0, sizeof(*out));
    out->uid = uid;
    strncpy(out->name, name, SYS_USER_NAME_MAX - 1);
    out->is_root = is_root;
}

static s64 sys_users_whoami(sys_user_info_t *out) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    if (!uaccess_check(out, sizeof(*out))) return SYSCALL_ERR_FAULT;

    sys_user_info_t info;
    fill_user_info(&info, users_current_uid(), users_current_name(), users_current_is_root());
    memcpy(out, &info, sizeof(info));
    return 0;
}

static s64 sys_users_list(sys_user_info_t *out, u32 max_entries, u32 *out_count) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    if (max_entries > USERS_MAX) max_entries = USERS_MAX;
    if (max_entries > 0 && !uaccess_check(out, max_entries * (u32)sizeof(sys_user_info_t))) return SYSCALL_ERR_FAULT;
    if (out_count && !uaccess_check(out_count, sizeof(u32))) return SYSCALL_ERR_FAULT;

    u32 filled = 0;
    for (u32 i = 0; i < USERS_MAX && filled < max_entries; i++) {
        user_t *u = users_get_slot(i);
        if (!u) continue;
        sys_user_info_t info;
        fill_user_info(&info, u->uid, u->name, u->is_root);
        memcpy(&out[filled], &info, sizeof(info));
        filled++;
    }
    if (out_count) *out_count = users_count();
    return 0;
}

static s64 sys_users_authenticate(const char *name, const char *password) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    char kname[USER_NAME_MAX], kpass[USER_PASS_MAX];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;
    if (!copy_user_name(password, kpass, sizeof(kpass))) return SYSCALL_ERR_FAULT;

    return users_authenticate(kname, kpass) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_users_read_password(char *buf, u32 maxlen) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    if (maxlen == 0 || maxlen > USER_PASS_MAX) maxlen = USER_PASS_MAX;
    if (!uaccess_check(buf, maxlen)) return SYSCALL_ERR_FAULT;

    char kbuf[USER_PASS_MAX];
    u32 len = users_read_password(kbuf, sizeof(kbuf));
    if (len >= maxlen) len = maxlen - 1;
    memcpy(buf, kbuf, len);
    buf[len] = '\0';
    return (s64)len;
}

static s64 sys_users_passwd(const char *target, const char *current_pass, const char *new_pass) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    char ktarget[USER_NAME_MAX], kcurrent[USER_PASS_MAX], knew[USER_PASS_MAX];
    if (!copy_user_name(target, ktarget, sizeof(ktarget))) return SYSCALL_ERR_FAULT;
    if (!copy_user_name(new_pass, knew, sizeof(knew))) return SYSCALL_ERR_FAULT;
    /* current_pass может быть NULL (root меняет чужой пароль без него) -- см. общий комментарий у SYS_USERS_PASSWD */
    kcurrent[0] = '\0';
    if (current_pass && !copy_user_name(current_pass, kcurrent, sizeof(kcurrent))) return SYSCALL_ERR_FAULT;

    bool_t is_self = (strcmp(ktarget, users_current_name()) == 0);
    if (!is_self && !users_current_is_root()) return SYSCALL_ERR_PERM;
    if (!users_find(ktarget)) return SYSCALL_ERR_BADH;

    if (is_self && !users_current_is_root()) {
        if (!users_authenticate(ktarget, kcurrent)) return SYSCALL_ERR_BADH;
    }

    return users_set_password(ktarget, knew) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_users_add(const char *name, const char *password, bool_t is_root) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM;
    char kname[USER_NAME_MAX], kpass[USER_PASS_MAX];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;
    if (!copy_user_name(password, kpass, sizeof(kpass))) return SYSCALL_ERR_FAULT;

    return users_add(kname, kpass, is_root) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_users_del(const char *name) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM;
    char kname[USER_NAME_MAX];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return users_del(kname) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_users_su(const char *target, const char *password) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    char ktarget[USER_NAME_MAX], kpass[USER_PASS_MAX];
    if (!copy_user_name(target, ktarget, sizeof(ktarget))) return SYSCALL_ERR_FAULT;
    if (!users_find(ktarget)) return SYSCALL_ERR_BADH;

    if (!users_current_is_root()) {
        kpass[0] = '\0';
        if (!copy_user_name(password, kpass, sizeof(kpass))) return SYSCALL_ERR_FAULT;
        if (!users_authenticate(ktarget, kpass)) return SYSCALL_ERR_BADH;
    }

    return users_switch_current(ktarget) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_users_logout(void) {
    if (!(task_current()->caps & CAP_USERS)) return SYSCALL_ERR_PERM;
    users_login_prompt(); /* блокирует, пока не введут верную пару логин/пароль -- см. include/sysabi.h */
    return 0;
}

/* ---- Диски/USB/время (Фаза 5, подшаг 5.4, см. include/sysabi.h за
   общим описанием этого блока). ---- */

static s64 sys_storage_list(sys_storage_info_t *out, u32 max_entries, u32 *out_count) {
    if (!(task_current()->caps & CAP_SYSINFO)) return SYSCALL_ERR_PERM;
    if (max_entries > STORAGE_MAX_DRIVES) max_entries = STORAGE_MAX_DRIVES;
    if (max_entries > 0 && !uaccess_check(out, max_entries * (u32)sizeof(sys_storage_info_t))) return SYSCALL_ERR_FAULT;
    if (out_count && !uaccess_check(out_count, sizeof(u32))) return SYSCALL_ERR_FAULT;

    u32 total = storage_drive_count();
    u32 to_copy = total < max_entries ? total : max_entries;
    for (u32 i = 0; i < to_copy; i++) {
        storage_dev_t *d = storage_get_drive(i);
        if (!d) break;

        sys_storage_info_t info;
        memset(&info, 0, sizeof(info));
        info.present = d->present;
        info.bus = (u32)d->bus;
        info.sectors = d->sectors;
        strncpy(info.model, d->model, sizeof(info.model) - 1);
        strncpy(info.location, d->location, sizeof(info.location) - 1);
        memcpy(&out[i], &info, sizeof(info));
    }
    if (out_count) *out_count = total;
    return 0;
}

static s64 sys_usb_list(sys_usb_info_t *out, u32 max_entries, u32 *out_count) {
    if (!(task_current()->caps & CAP_SYSINFO)) return SYSCALL_ERR_PERM;
    if (max_entries > 32) max_entries = 32; /* с запасом хватает -- корневой хаб UHCI и так ограничен числом портов */
    if (max_entries > 0 && !uaccess_check(out, max_entries * (u32)sizeof(sys_usb_info_t))) return SYSCALL_ERR_FAULT;
    if (out_count && !uaccess_check(out_count, sizeof(u32))) return SYSCALL_ERR_FAULT;

    u32 total = usb_device_count();
    u32 to_copy = total < max_entries ? total : max_entries;
    for (u32 i = 0; i < to_copy; i++) {
        usb_device_t *d = usb_get_device(i);
        if (!d) break;

        sys_usb_info_t info;
        memset(&info, 0, sizeof(info));
        info.present = d->present;
        info.speed = (u32)d->speed;
        info.vendor_id = d->vendor_id;
        info.product_id = d->product_id;
        info.dev_class = d->dev_class;
        info.dev_subclass = d->dev_subclass;
        info.dev_protocol = d->dev_protocol;
        memcpy(&out[i], &info, sizeof(info));
    }
    if (out_count) *out_count = total;
    return 0;
}

static s64 sys_usb_mouse_state(sys_mouse_state_t *out) {
    if (!(task_current()->caps & CAP_SYSINFO)) return SYSCALL_ERR_PERM;
    if (!uaccess_check(out, sizeof(*out))) return SYSCALL_ERR_FAULT;

    sys_mouse_state_t info;
    memset(&info, 0, sizeof(info));
    info.present = usb_hid_mouse_present();
    if (info.present) {
        u8 buttons; s32 x, y;
        usb_hid_mouse_get_state(&buttons, &x, &y);
        info.buttons = buttons;
        info.x = x;
        info.y = y;
    }
    memcpy(out, &info, sizeof(info));
    return 0;
}

static s64 sys_rtc_read(sys_rtc_time_t *out) {
    if (!uaccess_check(out, sizeof(*out))) return SYSCALL_ERR_FAULT;

    rtc_time_t t;
    rtc_read(&t);
    sys_rtc_time_t kout;
    kout.second = t.second;
    kout.minute = t.minute;
    kout.hour = t.hour;
    kout.day = t.day;
    kout.month = t.month;
    kout.year = t.year;
    memcpy(out, &kout, sizeof(kout));
    return 0;
}

static s64 sys_fs_mount(u32 drive_index) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    return fs_mount(drive_index) ? 0 : SYSCALL_ERR_BADH;
}

/* ---- Сырой блочный доступ к диску, в обход VFS (см. общий комментарий
   у SYS_DISK_READ_RAW/SYS_DISK_WRITE_RAW, include/sysabi.h, за тем,
   зачем это вообще нужно, и у CAP_DISK_RAW, include/container.h, за
   тем, почему это отдельная и самая опасная capability).

   storage_read_sectors()/storage_write_sectors() (include/storage.h)
   принимают count как u8 (максимум 255 секторов за вызов драйвера) --
   поэтому большой запрос от userspace режем на такие чанки здесь, а не
   расширяем ABI storage.h ради одного вызывающего. */
static s64 sys_disk_read_raw(u32 drive_index, u32 lba, u32 count, void *buf) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM; /* см. общий комментарий у CAP_DISK_RAW, include/container.h */
    if (count == 0) return 0;
    if (count > SYS_DISK_MAX_SECTORS_PER_CALL) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, count * 512u)) return SYSCALL_ERR_FAULT;

    u8 *p = (u8 *)buf;
    u32 remaining = count;
    u32 cur_lba = lba;
    while (remaining > 0) {
        u8 chunk = remaining > 255 ? 255 : (u8)remaining;
        if (!storage_read_sectors(drive_index, cur_lba, chunk, p)) return SYSCALL_ERR_BADH;
        p += (u32)chunk * 512u;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

static s64 sys_disk_write_raw(u32 drive_index, u32 lba, u32 count, const void *buf) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM; /* см. общий комментарий у CAP_DISK_RAW, include/container.h */
    if (count == 0) return 0;
    if (count > SYS_DISK_MAX_SECTORS_PER_CALL) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, count * 512u)) return SYSCALL_ERR_FAULT;

    const u8 *p = (const u8 *)buf;
    u32 remaining = count;
    u32 cur_lba = lba;
    while (remaining > 0) {
        u8 chunk = remaining > 255 ? 255 : (u8)remaining;
        if (!storage_write_sectors(drive_index, cur_lba, chunk, p)) return SYSCALL_ERR_BADH;
        p += (u32)chunk * 512u;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* ---- Boot-ассеты для install-os -- см. общий комментарий у
   SYS_BOOT_ASSET_READ (include/sysabi.h) и include/boot_assets.h за
   тем, почему это не RAMFS-файл. ---- */
static s64 sys_boot_asset_read(u32 asset_id, void *buf, u32 bufsize, u32 *out_size) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;

    const u8 *data;
    u32 size;
    if (!boot_asset_get((boot_asset_id_t)asset_id, &data, &size)) return SYSCALL_ERR_BADH;
    if (size > bufsize) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, size)) return SYSCALL_ERR_FAULT;
    if (!uaccess_check(out_size, sizeof(u32))) return SYSCALL_ERR_FAULT;

    memcpy(buf, data, size);
    *out_size = size;
    return 0;
}

/* См. общий комментарий у SYS_BOOT_ASSET_WRITE_TO_DISK/VERIFY_ON_DISK,
 * include/sysabi.h, за тем, почему эти два syscall'а вообще понадобились
 * (kernel.flat давно перерос APP_MAX_SIZE) -- ЭТОТ размер чанка (128
 * секторов = 64 КиБ) выбран той же логикой, что CHUNK_SECTORS в
 * boot/stage2.s/stage2_atomic.s и ZERO_CHUNK_SECTORS в apps/install-os.c:
 * маленький статический буфер, не куча, кратно быстрее одного сектора
 * за раз, без нужды вмещать весь ассет разом. */
#define BOOT_ASSET_DISK_CHUNK_SECTORS 128u
static u8 g_boot_asset_disk_chunk[BOOT_ASSET_DISK_CHUNK_SECTORS * 512u];

static s64 sys_boot_asset_write_to_disk(u32 asset_id, u32 drive_index, u32 lba_start, u32 region_sectors) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;

    const u8 *data;
    u32 size;
    if (!boot_asset_get((boot_asset_id_t)asset_id, &data, &size)) return SYSCALL_ERR_BADH;

    u32 real_sectors = (size + 511u) / 512u;
    if (real_sectors > region_sectors) return SYSCALL_ERR_TOOBIG;

    u32 offset = 0;
    u32 remaining_sectors = real_sectors;
    u32 lba = lba_start;
    while (remaining_sectors > 0) {
        u32 chunk = (remaining_sectors > BOOT_ASSET_DISK_CHUNK_SECTORS) ? BOOT_ASSET_DISK_CHUNK_SECTORS : remaining_sectors;
        /* Последний чанк ассета может быть короче chunk*512 байт (size не
           обязан делиться на 512 нацело) -- дополняем хвост нулями в
           СОБСТВЕННОМ статическом буфере (не пишем прямо из .rodata,
           когда чанк неполный, иначе унесли бы на диск byte'ы ЗА
           пределами данных ассета -- за .rodata ядра, вообще говоря, не
           гарантировано ничего осмысленного). Полные чанки -- пишем
           НАПРЯМУЮ из data+offset, копия не нужна вовсе. */
        const void *src;
        if (offset + (u64)chunk * 512u <= size) {
            src = data + offset;
        } else {
            u32 valid_bytes = size - offset;
            memcpy(g_boot_asset_disk_chunk, data + offset, valid_bytes);
            memset(g_boot_asset_disk_chunk + valid_bytes, 0, (u32)chunk * 512u - valid_bytes);
            src = g_boot_asset_disk_chunk;
        }
        if (!storage_write_sectors(drive_index, lba, (u8)chunk, src)) return SYSCALL_ERR_BADH;
        offset += chunk * 512u;
        lba += chunk;
        remaining_sectors -= chunk;
    }

    /* Зануляем остаток region_sectors ПОСЛЕ реального размера ассета --
       см. общий комментарий у этого syscall'а, include/sysabi.h. */
    u32 padding_sectors = region_sectors - real_sectors;
    if (padding_sectors > 0) {
        memset(g_boot_asset_disk_chunk, 0, sizeof(g_boot_asset_disk_chunk));
        while (padding_sectors > 0) {
            u32 chunk = (padding_sectors > BOOT_ASSET_DISK_CHUNK_SECTORS) ? BOOT_ASSET_DISK_CHUNK_SECTORS : padding_sectors;
            if (!storage_write_sectors(drive_index, lba, (u8)chunk, g_boot_asset_disk_chunk)) return SYSCALL_ERR_BADH;
            lba += chunk;
            padding_sectors -= chunk;
        }
    }

    return (s64)size;
}

static s64 sys_boot_asset_verify_on_disk(u32 asset_id, u32 drive_index, u32 lba_start) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;

    const u8 *data;
    u32 size;
    if (!boot_asset_get((boot_asset_id_t)asset_id, &data, &size)) return SYSCALL_ERR_BADH;

    u32 source_crc = crc32(0, data, size); /* весь ассет разом -- уже в .rodata ядра, чанковать незачем */

    u32 real_sectors = (size + 511u) / 512u;
    u32 running_crc = 0;
    u32 bytes_remaining = size;
    u32 remaining_sectors = real_sectors;
    u32 lba = lba_start;
    while (remaining_sectors > 0) {
        u32 chunk = (remaining_sectors > BOOT_ASSET_DISK_CHUNK_SECTORS) ? BOOT_ASSET_DISK_CHUNK_SECTORS : remaining_sectors;
        if (!storage_read_sectors(drive_index, lba, (u8)chunk, g_boot_asset_disk_chunk)) return SYSCALL_ERR_BADH;
        u32 chunk_bytes = chunk * 512u;
        u32 valid_bytes = (chunk_bytes < bytes_remaining) ? chunk_bytes : bytes_remaining; /* хвостовой чанк короче
                                                                                                chunk*512 -- см. общий
                                                                                                комментарий у "write" выше */
        running_crc = crc32(running_crc, g_boot_asset_disk_chunk, valid_bytes);
        bytes_remaining -= valid_bytes;
        lba += chunk;
        remaining_sectors -= chunk;
    }

    return (running_crc == source_crc) ? 0 : SYSCALL_ERR_BADH;
}

/* См. общий комментарий у SYS_FS_FORMAT_HOME_ENCRYPTED, include/sysabi.h,
   и у ufs_format_encrypted(), fs/ufs.c. */
static s64 sys_fs_format_home_encrypted(u32 drive_index, u32 slot_body_lba_start,
                                         u32 slot_body_sectors, const u8 *master_key) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM; /* см. общий комментарий у CAP_DISK_RAW, include/container.h */
    if (!uaccess_check(master_key, UFS_MASTER_KEY_LEN)) return SYSCALL_ERR_FAULT;

    u8 kkey[UFS_MASTER_KEY_LEN];
    memcpy(kkey, master_key, UFS_MASTER_KEY_LEN); /* копия в память ядра -- та же дисциплина, что copy_user_name()/kbuf выше в этом файле, не работаем с указателем в адресном пространстве вызывающего дольше необходимого */

    bool_t ok = fs_format_home_encrypted(drive_index, slot_body_lba_start, slot_body_sectors, kkey);
    memset(kkey, 0, sizeof(kkey)); /* best-effort стереть копию ключа со стека ядра */
    return ok ? 0 : SYSCALL_ERR_BADH;
}

/* См. общий комментарий у SYS_BOOT_CONFIRM, include/sysabi.h, и у
   bootconfirm_mark_ok(), kernel/bootconfirm.c. Гейт -- та же двойная
   защита (capability И users_current_is_root()), что и у соседних
   диск-модифицирующих syscall'ов выше, хотя сама операция (в отличие от
   них) не разрушительна -- см. её комментарий за тем, почему. */
static s64 sys_boot_confirm(void) {
    if (!(task_current()->caps & CAP_DISK_RAW)) return SYSCALL_ERR_PERM;
    if (!users_current_is_root()) return SYSCALL_ERR_PERM;
    return bootconfirm_mark_ok() ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_format(u32 drive_index, u32 fs_type) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    if (fs_type != FS_TYPE_FAT32 && fs_type != FS_TYPE_UFS) return SYSCALL_ERR_BADH;
    return fs_format(drive_index, (fs_type_t)fs_type) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_chmod(const char *name, u32 mode) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return fs_chmod(kname, mode) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_chown(const char *name, u32 new_uid) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    char kname[256];
    if (!copy_user_name(name, kname, sizeof(kname))) return SYSCALL_ERR_FAULT;

    return fs_chown(kname, new_uid) ? 0 : SYSCALL_ERR_BADH;
}

static s64 sys_fs_backend_info(sys_fs_backend_info_t *out) {
    if (!(task_current()->caps & CAP_FS_READ)) return SYSCALL_ERR_PERM;
    if (!uaccess_check(out, sizeof(*out))) return SYSCALL_ERR_FAULT;

    sys_fs_backend_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, fs_backend_name(), sizeof(info.name) - 1);
    info.is_mounted = fs_is_mounted();
    memcpy(out, &info, sizeof(info));
    return 0;
}

static s64 sys_net_set_static(u32 ip, u32 netmask, u32 gateway, u32 dns) {
    if (!(task_current()->caps & CAP_NET)) return SYSCALL_ERR_PERM;
    net_set_static((ip_addr_t)ip, (ip_addr_t)netmask, (ip_addr_t)gateway, (ip_addr_t)dns);
    return 0;
}

/* ---- Фаза 5, подшаг 5.5: см. общий комментарий у SYS_CLEAR/
   SYS_FS_USE_RAMFS в include/sysabi.h. ---- */

static s64 sys_clear(void) {
    vga_clear();
    return 0;
}

static s64 sys_fs_use_ramfs(void) {
    if (!(task_current()->caps & CAP_FS_WRITE)) return SYSCALL_ERR_PERM;
    fs_use_ramfs();
    return 0;
}

/* ---- контейнеры (см. include/container.h, SYS_CONTAINER_SPAWN в
   include/sysabi.h) ---- */

static s64 sys_container_spawn(const char *manifest_path, u32 argc, char **argv) {
    task_t *cur = task_current();
    if (!(cur->caps & CAP_SPAWN)) return SYSCALL_ERR_PERM;

    char kpath[CONTAINER_PATH_MAX];
    if (!copy_user_name(manifest_path, kpath, sizeof(kpath))) return SYSCALL_ERR_FAULT;

    char **kargv = NULL;
    s64 aerr = copy_user_argv(argc, argv, &kargv);
    if (aerr < 0) return aerr;

    container_manifest_t m;
    if (!container_manifest_load(kpath, &m)) {
        free_kargv(argc, kargv);
        return SYSCALL_ERR_FAULT;
    }

    /* CAP_SPAWN разрешает породить контейнер -- не обойти собственные
       ограничения через манифест с caps=all. Дочерние caps ДОЛЖНЫ быть
       подмножеством caps вызывающего (m.caps & ~cur->caps == 0); иначе
       контейнер без, скажем, CAP_NET мог бы просто написать манифест,
       просящий caps=all, и получить сеть в обход своего же манифеста --
       ровно то, чего вся модель Фазы 1/2 должна не допускать.

       ИСКЛЮЧЕНИЕ -- root (см. users_current_is_root(), kernel/users.c):
       та же логика, что уже применена к sys_users_add()/chown() выше --
       root аутентифицирован ОТДЕЛЬНЫМ механизмом (логин/пароль,
       kernel/users.c), не через caps контейнера-родителя, поэтому
       способность запросить манифест с БОЛЬШИМИ правами, чем у текущего
       контейнера (например, install-os с CAP_DISK_RAW из-под caps=0xf8f
       обычного шелла -- см. INSTALLER_PLAN.md, Фаза 4) -- это не дыра в
       модели "контейнер не может дать себе больше прав, чем имеет", а
       её продолжение: root и так уже может часть эту модель обойти
       другими способами (создать пользователя с любыми правами,
       сменить владельца любого файла) -- будь у 'rungod' (см. общий
       комментарий в apps/shell.c про его осознанное удаление) отличие
       от этого исключения, оно было бы в том, что rungod выдавал
       ПОЛНЫЙ ring-0 доступ в обход int 0x80 целиком; это исключение,
       наоборот, оставляет ребёнка обычным ring-3 процессом, ограниченным
       syscall'ами -- просто с более широким набором РАЗРЕШЁННЫХ
       syscall'ов, чем у родителя. НЕ root -- проверка ведёт себя ровно
       как раньше, без исключений. */
    if ((m.caps & ~cur->caps) && !users_current_is_root()) {
        kprintf("container: spawn denied -- manifest '%s' requests caps 0x%x beyond caller's own 0x%x\n",
                kpath, m.caps, cur->caps);
        free_kargv(argc, kargv);
        return SYSCALL_ERR_PERM;
    }

    /* CAP_IO -- capability с параметрами (диапазон портов), а не просто
       бит: одного лишь совпадения битов CAP_* недостаточно (CAP_ALL у
       полностью доверенного 'run'/'rungod' формально включает и бит
       CAP_IO, но t->io_port_len при этом 0 -- см. process_run_sandboxed()
       в kernel/process.c). Ребёнок обязан просить диапазон, целиком
       лежащий ВНУТРИ диапазона самого вызывающего -- иначе контейнер с
       портами только своей сетевой карты мог бы породить "ребёнка" с
       io_base=0/io_len=0x10000 (всё пространство портов) и получить
       доступ к чужому железу в обход собственного манифеста. */
    if (m.caps & CAP_IO) {
        u32 child_end = (u32)m.io_base + (u32)m.io_len;
        u32 parent_end = (u32)cur->io_port_base + (u32)cur->io_port_len;
        if (m.io_len == 0 || m.io_base < cur->io_port_base || child_end > parent_end) {
            kprintf("container: spawn denied -- manifest '%s' requests I/O ports [0x%x,0x%x) beyond caller's own [0x%x,0x%x)\n",
                    kpath, m.io_base, child_end, cur->io_port_base, parent_end);
            free_kargv(argc, kargv);
            return SYSCALL_ERR_PERM;
        }
    }

    /* CAP_IRQ -- тоже capability с параметром, но параметр здесь --
       ОДНА линия, не диапазон: точное совпадение, а не "подмножество".
       Ребёнок обязан просить ТУ ЖЕ линию, что уже разрешена самому
       вызывающему (а не просто иметь бит CAP_IRQ где-то в caps) -- иначе
       контейнер с IRQ своей сетевой карты мог бы породить "ребёнка",
       слушающего, скажем, линию клавиатуры, к которой сам никакого
       отношения не имеет. */
    if (m.caps & CAP_IRQ) {
        if (m.irq_line != cur->irq_line) {
            kprintf("container: spawn denied -- manifest '%s' requests irq %u, caller only has irq %u\n",
                    kpath, m.irq_line, cur->irq_line);
            free_kargv(argc, kargv);
            return SYSCALL_ERR_PERM;
        }
    }

    /* argv[0] дочернего контейнера -- всегда путь бинарника из манифеста,
       как и у 'runc' в shell/shell.c; то, что передал вызывающий сюда
       (kargv/argc) -- это argv[1..]. */
    char *final_argv[129];
    u32 final_argc = 0;
    final_argv[final_argc++] = m.path;
    for (u32 i = 0; i < argc && final_argc < 129; i++) final_argv[final_argc++] = kargv[i];

    s64 ret = container_spawn(&m, final_argc, final_argv);
    free_kargv(argc, kargv);
    return ret;
}

/* ---- IRQ (см. include/irqbridge.h, SYS_IRQ_WAIT в include/sysabi.h) ---- */

static s64 sys_irq_wait(u32 irq) {
    task_t *cur = task_current();
    if (!(cur->caps & CAP_IRQ)) return SYSCALL_ERR_PERM;
    if (irq > 15 || (u32)cur->irq_line != irq) return SYSCALL_ERR_PERM;

    return irqbridge_wait((u8)irq) ? 0 : SYSCALL_ERR_PERM;
}

/* ---- мост кадров ядро<->драйвер-контейнер (см. include/netbridge.h,
   SYS_NETBRIDGE_* в include/sysabi.h) ---- */

static s64 sys_netbridge_tx_dequeue(void *buf, u32 maxlen) {
    task_t *cur = task_current();
    if (!(cur->caps & CAP_NET_BRIDGE)) return SYSCALL_ERR_PERM;
    if (maxlen > NETBRIDGE_MAX_FRAME) maxlen = NETBRIDGE_MAX_FRAME;
    if (!uaccess_check(buf, maxlen)) return SYSCALL_ERR_FAULT;

    return netbridge_sys_tx_dequeue(buf, maxlen);
}

static s64 sys_netbridge_rx_enqueue(const void *buf, u32 len) {
    task_t *cur = task_current();
    if (!(cur->caps & CAP_NET_BRIDGE)) return SYSCALL_ERR_PERM;
    if (len > NETBRIDGE_MAX_FRAME) return SYSCALL_ERR_TOOBIG;
    if (!uaccess_check(buf, len)) return SYSCALL_ERR_FAULT;

    return netbridge_sys_rx_enqueue(buf, len);
}

void syscall_isr_handler(registers_t *regs) {
    switch (regs->rax) {
        case SYS_EXIT:
            process_exit((s32)regs->rdi); /* не возвращается */
            return; /* не достигается */

        case SYS_FORK:
            regs->rax = (u64)process_fork(regs);
            break;

        case SYS_WRITE:
            regs->rax = (u64)sys_write((const char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_READLINE:
            regs->rax = (u64)sys_readline((char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_GETCHAR:
            regs->rax = (u64)(s64)keyboard_getchar();
            break;

        case SYS_YIELD:
            task_yield();
            regs->rax = 0;
            break;

        case SYS_MEMINFO:
            regs->rax = (u64)sys_meminfo((sys_meminfo_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_PIPE_CREATE:
            regs->rax = (u64)sys_pipe_create((s32 *)(uintptr_t)regs->rdi);
            break;

        case SYS_PIPE_READ:
            regs->rax = (u64)sys_pipe_read((s32)regs->rdi, (void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_PIPE_WRITE:
            regs->rax = (u64)sys_pipe_write((s32)regs->rdi, (const void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_PIPE_CLOSE:
            regs->rax = (u64)sys_pipe_close((s32)regs->rdi);
            break;

        case SYS_MQ_CREATE:
            regs->rax = (u64)sys_mq_create((const char *)(uintptr_t)regs->rdi);
            break;

        case SYS_MQ_SEND:
            regs->rax = (u64)sys_mq_send((s32)regs->rdi, (const void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_MQ_RECV:
            regs->rax = (u64)sys_mq_recv((s32)regs->rdi, (void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_MQ_CLOSE:
            regs->rax = (u64)sys_mq_close((s32)regs->rdi);
            break;

        case SYS_SHM_CREATE:
            regs->rax = (u64)sys_shm_create((const char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_SHM_ATTACH:
            regs->rax = (u64)sys_shm_attach((s32)regs->rdi, (void **)(uintptr_t)regs->rsi);
            break;

        case SYS_SHM_DETACH:
            regs->rax = (u64)sys_shm_detach((void *)(uintptr_t)regs->rdi);
            break;

        case SYS_SHM_DESTROY:
            regs->rax = (u64)sys_shm_destroy((s32)regs->rdi);
            break;

        case SYS_EXECVE:
            regs->rax = (u64)sys_execve((const char *)(uintptr_t)regs->rdi, (u32)regs->rsi, (char **)(uintptr_t)regs->rdx, regs);
            break;

        case SYS_WAITPID:
            regs->rax = (u64)sys_waitpid((s32)regs->rdi, (s32 *)(uintptr_t)regs->rsi);
            break;

        case SYS_NET_STATUS:
            regs->rax = (u64)sys_net_status((sys_net_status_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_NET_DHCP:
            regs->rax = (u64)sys_net_dhcp((u32)regs->rdi);
            break;

        case SYS_NET_RESOLVE:
            regs->rax = (u64)sys_net_resolve((const char *)(uintptr_t)regs->rdi, (u32 *)(uintptr_t)regs->rsi);
            break;

        case SYS_NET_PING:
            regs->rax = (u64)sys_net_ping((u32)regs->rdi, (u32)regs->rsi, (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_TCP_CONNECT:
            regs->rax = (u64)sys_tcp_connect((u32)regs->rdi, (u16)regs->rsi);
            break;

        case SYS_TCP_SEND:
            regs->rax = (u64)sys_tcp_send((s32)regs->rdi, (const void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_TCP_RECV:
            regs->rax = (u64)sys_tcp_recv((s32)regs->rdi, (void *)(uintptr_t)regs->rsi, (u32)regs->rdx, (u32)regs->rcx);
            break;

        case SYS_TCP_CLOSE:
            regs->rax = (u64)sys_tcp_close((s32)regs->rdi);
            break;

        case SYS_FS_WRITE_FILE:
            regs->rax = (u64)sys_fs_write_file((const char *)(uintptr_t)regs->rdi,
                                                (const void *)(uintptr_t)regs->rsi, (u32)regs->rdx);
            break;

        case SYS_FS_READ_FILE:
            regs->rax = (u64)sys_fs_read_file((const char *)(uintptr_t)regs->rdi,
                                               (void *)(uintptr_t)regs->rsi, (u32)regs->rdx,
                                               (u32 *)(uintptr_t)regs->rcx);
            break;

        case SYS_FS_STAT:
            regs->rax = (u64)sys_fs_stat((const char *)(uintptr_t)regs->rdi,
                                          (sys_fs_stat_t *)(uintptr_t)regs->rsi);
            break;

        case SYS_FS_PWD:
            regs->rax = (u64)sys_fs_pwd((char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_FS_CD:
            regs->rax = (u64)sys_fs_cd((const char *)(uintptr_t)regs->rdi);
            break;

        case SYS_FS_MKDIR:
            regs->rax = (u64)sys_fs_mkdir((const char *)(uintptr_t)regs->rdi);
            break;

        case SYS_FS_RM:
            regs->rax = (u64)sys_fs_rm((const char *)(uintptr_t)regs->rdi);
            break;

        case SYS_FS_LIST:
            regs->rax = (u64)sys_fs_list((sys_fs_dirent_t *)(uintptr_t)regs->rdi, (u32)regs->rsi,
                                          (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_PS_LIST:
            regs->rax = (u64)sys_ps_list((sys_task_info_t *)(uintptr_t)regs->rdi, (u32)regs->rsi,
                                          (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_SLEEP:
            regs->rax = (u64)sys_sleep((u32)regs->rdi);
            break;

        case SYS_POWER_REBOOT:
            regs->rax = (u64)sys_power_reboot();
            break;

        case SYS_POWER_SHUTDOWN:
            regs->rax = (u64)sys_power_shutdown();
            break;

        case SYS_USERS_WHOAMI:
            regs->rax = (u64)sys_users_whoami((sys_user_info_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_USERS_LIST:
            regs->rax = (u64)sys_users_list((sys_user_info_t *)(uintptr_t)regs->rdi, (u32)regs->rsi,
                                             (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_USERS_AUTHENTICATE:
            regs->rax = (u64)sys_users_authenticate((const char *)(uintptr_t)regs->rdi,
                                                      (const char *)(uintptr_t)regs->rsi);
            break;

        case SYS_USERS_READ_PASSWORD:
            regs->rax = (u64)sys_users_read_password((char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_USERS_PASSWD:
            regs->rax = (u64)sys_users_passwd((const char *)(uintptr_t)regs->rdi,
                                               (const char *)(uintptr_t)regs->rsi,
                                               (const char *)(uintptr_t)regs->rdx);
            break;

        case SYS_USERS_ADD:
            regs->rax = (u64)sys_users_add((const char *)(uintptr_t)regs->rdi,
                                            (const char *)(uintptr_t)regs->rsi, (bool_t)regs->rdx);
            break;

        case SYS_USERS_DEL:
            regs->rax = (u64)sys_users_del((const char *)(uintptr_t)regs->rdi);
            break;

        case SYS_USERS_SU:
            regs->rax = (u64)sys_users_su((const char *)(uintptr_t)regs->rdi,
                                           (const char *)(uintptr_t)regs->rsi);
            break;

        case SYS_USERS_LOGOUT:
            regs->rax = (u64)sys_users_logout();
            break;

        case SYS_STORAGE_LIST:
            regs->rax = (u64)sys_storage_list((sys_storage_info_t *)(uintptr_t)regs->rdi, (u32)regs->rsi,
                                               (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_USB_LIST:
            regs->rax = (u64)sys_usb_list((sys_usb_info_t *)(uintptr_t)regs->rdi, (u32)regs->rsi,
                                           (u32 *)(uintptr_t)regs->rdx);
            break;

        case SYS_USB_MOUSE_STATE:
            regs->rax = (u64)sys_usb_mouse_state((sys_mouse_state_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_RTC_READ:
            regs->rax = (u64)sys_rtc_read((sys_rtc_time_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_FS_MOUNT:
            regs->rax = (u64)sys_fs_mount((u32)regs->rdi);
            break;

        case SYS_FS_FORMAT:
            regs->rax = (u64)sys_fs_format((u32)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_FS_CHMOD:
            regs->rax = (u64)sys_fs_chmod((const char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_FS_CHOWN:
            regs->rax = (u64)sys_fs_chown((const char *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_FS_BACKEND_INFO:
            regs->rax = (u64)sys_fs_backend_info((sys_fs_backend_info_t *)(uintptr_t)regs->rdi);
            break;

        case SYS_NET_SET_STATIC:
            regs->rax = (u64)sys_net_set_static((u32)regs->rdi, (u32)regs->rsi, (u32)regs->rdx, (u32)regs->rcx);
            break;

        case SYS_CONTAINER_SPAWN:
            regs->rax = (u64)sys_container_spawn((const char *)(uintptr_t)regs->rdi,
                                                  (u32)regs->rsi, (char **)(uintptr_t)regs->rdx);
            break;

        case SYS_IRQ_WAIT:
            regs->rax = (u64)sys_irq_wait((u32)regs->rdi);
            break;

        case SYS_NETBRIDGE_TX_DEQUEUE:
            regs->rax = (u64)sys_netbridge_tx_dequeue((void *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_NETBRIDGE_RX_ENQUEUE:
            regs->rax = (u64)sys_netbridge_rx_enqueue((const void *)(uintptr_t)regs->rdi, (u32)regs->rsi);
            break;

        case SYS_CLEAR:
            regs->rax = (u64)sys_clear();
            break;

        case SYS_FS_USE_RAMFS:
            regs->rax = (u64)sys_fs_use_ramfs();
            break;

        case SYS_DISK_READ_RAW:
            regs->rax = (u64)sys_disk_read_raw((u32)regs->rdi, (u32)regs->rsi,
                                                (u32)regs->rdx, (void *)(uintptr_t)regs->rcx);
            break;

        case SYS_DISK_WRITE_RAW:
            regs->rax = (u64)sys_disk_write_raw((u32)regs->rdi, (u32)regs->rsi,
                                                 (u32)regs->rdx, (const void *)(uintptr_t)regs->rcx);
            break;

        case SYS_BOOT_ASSET_READ:
            regs->rax = (u64)sys_boot_asset_read((u32)regs->rdi, (void *)(uintptr_t)regs->rsi,
                                                  (u32)regs->rdx, (u32 *)(uintptr_t)regs->rcx);
            break;

        case SYS_FS_FORMAT_HOME_ENCRYPTED:
            regs->rax = (u64)sys_fs_format_home_encrypted((u32)regs->rdi, (u32)regs->rsi,
                                                            (u32)regs->rdx, (const u8 *)(uintptr_t)regs->rcx);
            break;

        case SYS_BOOT_CONFIRM:
            regs->rax = (u64)sys_boot_confirm();
            break;

        case SYS_BOOT_ASSET_WRITE_TO_DISK:
            regs->rax = (u64)sys_boot_asset_write_to_disk((u32)regs->rdi, (u32)regs->rsi,
                                                            (u32)regs->rdx, (u32)regs->rcx);
            break;

        case SYS_BOOT_ASSET_VERIFY_ON_DISK:
            regs->rax = (u64)sys_boot_asset_verify_on_disk((u32)regs->rdi, (u32)regs->rsi, (u32)regs->rdx);
            break;

        default:
            regs->rax = (u64)(s64)SYSCALL_ERR_NOSYS;
            break;
    }
}

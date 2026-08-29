/* shell.c -- интерактивный шелл MeetlookCoreOS как обычный ring-3
 * контейнер (Фаза 5, подшаг 5.5, см. CONTAINER_ARCHITECTURE.md).
 *
 * До этого файла shell/shell.c линковался прямо в kernel.bin и вызывал
 * ~50 функций ядра напрямую (fs_ls(), users_login_prompt(),
 * container_spawn(), net_dhcp_request()...) -- то есть был доверенным
 * кодом ядра, а не sandboxed-приложением, ровно как когда-то
 * drivers/rtl8139.c (см. Фазу 4, подшаг 4). Этот файл -- то, чем стал
 * shell после переноса: обычный APP_ENTRY app_main(), который умеет
 * говорить с ядром ТОЛЬКО через int 0x80 (include/usyscall.h,
 * include/sysabi.h), запускается через container_spawn() из
 * kernel_main() (kernel/kernel.c) с манифестом, объединяющим caps всех
 * подшагов 5.1-5.4 (см. include/container.h), и не более доверен, чем
 * любой другой контейнер со своим манифестом.
 *
 * ---- Что НЕ перенесено из shell/shell.c, и почему (честно, как и
 * весь остальной документ) ----
 *
 * - 'rungod' -- запускал бинарник в ring 0 с полным доступом kapi_t
 *   (include/kapi.h). Это не забытая деталь: КОНТЕЙНЕР (а именно им
 *   теперь является сам шелл) в принципе не может выдать ring-0
 *   доступ никому, включая себя -- это разрушило бы всю модель Фаз
 *   1-5 целиком (контейнер с caps=all мог бы тривиально дать себе
 *   ring 0 через 'rungod', и вся капабилити-система была бы
 *   декоративной). Соответствующего SYS_RUNGOD в include/sysabi.h
 *   сознательно нет и не будет. Бинарники вроде старого /bin/calc,
 *   которые всё ещё зовут kapi_t напрямую (а не include/usyscall.h),
 *   с этой фазы не запускаются интерактивно -- это осознанная потеря
 *   функциональности, а не забытая, см. общий комментарий в конце
 *   cmd_help() ниже.
 * - 'synctest' -- дёргал task_create()/mutex_t/semaphore_t/condvar_t
 *   (kernel/sync.c, kernel/task.c) НАПРЯМУЮ, как код ядра. Никакого
 *   syscall'а "создать сырую задачу ядра из ring 3" нет и не должно
 *   быть -- это была самопроверка примитивов синхронизации ядра, а не
 *   пользовательская функциональность, ей нет естественного места в
 *   ring-3 ABI.
 * - 'nbtest' -- тестовый хук Фазы 4, подшаг 3 (класть текст как "кадр"
 *   в TX-очередь netbridge в обход настоящего сетевого стека) --
 *   полностью вытеснен настоящим драйвером-контейнером
 *   (apps/rtl8139d.c, Фаза 4.4): у обычного пользователя нет причин
 *   класть кадры в этот канал напрямую, а тот, кто ДОЛЖЕН это делать
 *   (сам rtl8139d), не нуждается в шелле для этого.
 * - демо-счётчики 'demo-a'/'demo-b' в выводе 'ps' -- были статическими
 *   переменными kernel/kernel.c, к которым shell/shell.c обращался
 *   через extern-функции. Ring 3 не имеет и не должен иметь прямого
 *   доступа к произвольным static-переменным ядра -- SYS_PS_LIST
 *   (include/sysabi.h) всё ещё показывает сами задачи demo-a/demo-b
 *   в списке (id/имя/состояние), просто без их внутренних счётчиков.
 *
 * ---- 'run': что изменилось по сравнению с 'run' в shell/shell.c ----
 *
 * Старый 'run' звал loader_run_file_sandboxed() -- функцию ядра,
 * которая давала CAP_ALL (полное доверие ring 3) БЕЗ манифеста: это
 * было безопасно ровно потому, что сам шелл был кодом ядра и мог
 * выдавать любые права. Теперь шелл сам -- обычный контейнер со своим
 * ограниченным набором caps (см. манифест, который строит kernel_main()
 * в kernel/kernel.c) -- он физически не может выдать ребёнку больше
 * прав, чем есть у него самого (см. проверку "подмножество" в
 * sys_container_spawn(), kernel/syscall.c). Поэтому новый 'run'
 * генерирует манифест на лету (см. spawn_with_shell_caps() ниже) с
 * РОВНО ТЕМИ ЖЕ caps, что есть у самого шелла -- это лучшее, что шелл
 * вообще может предложить, и семантически ближе к "как раньше", чем
 * заставлять всех переходить на 'runc' с явным манифестом для КАЖДОГО
 * бинарника, включая безобидные hello/forktest/ipctest. 'runc' по
 * прежнему доступен для явного, более узкого набора прав. */

#include "mlapp.h"
#include "libc.h"

#define OS_NAME "MeetlookCoreOS"
#define OS_VERSION "1.0.1"

#define MAX_LINE 256
#define MAX_ARGS 16

/* Скрэтч-файл для манифеста, который генерирует 'run' -- см.
   spawn_with_shell_caps() ниже. Имя начинается с '.', чтобы не
   мозолить глаза в обычном 'ls'; создаётся в ТЕКУЩЕМ каталоге (см.
   общий комментарий у CAP_FS_WRITE, include/container.h: и
   sys_fs_write_file(), и sys_container_spawn() разрешают путь
   относительно текущего каталога активной ФС -- оба вызова здесь
   происходят без cd между ними, так что каталог гарантированно тот
   же самый). */
#define RUN_MANIFEST_PATH ".run.manifest"

/* Ровно тот набор caps, что манифест шелла (kernel/kernel.c) выдаёт
   самому шеллу -- см. общий комментарий в начале файла про 'run'.
   ДОЛЖНО оставаться подмножеством (а практически -- точной копией)
   набора caps в манифесте шелла, иначе sys_container_spawn() будет
   просто всегда отказывать 'run' с SYSCALL_ERR_PERM. */
#define SHELL_CHILD_CAPS "net,fs_write,fs_read,shm,spawn,ps,users,power,sysinfo"

/* ---- мелкие вспомогательные функции ---- */

static void print_ip(u32 ip) {
    printf("%u.%u.%u.%u", (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu, ip & 0xFFu);
}

/* Простой разбор точечно-десятичной записи IPv4 -- include/dns.h
   (dns_parse_ipv4_literal()) недоступен ring-3 приложениям (это
   функция ядра, а не часть usyscall.h), так что здесь -- своя
   маленькая копия той же идеи. */
static bool_t parse_ipv4(const char *s, u32 *out) {
    u32 parts[4];
    u32 pi = 0;
    const char *p = s;
    while (pi < 4) {
        if (*p < '0' || *p > '9') return FALSE;
        u32 v = 0;
        u32 digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (u32)(*p - '0');
            digits++;
            p++;
            if (digits > 3 || v > 255) return FALSE;
        }
        parts[pi++] = v;
        if (pi < 4) {
            if (*p != '.') return FALSE;
            p++;
        }
    }
    if (*p != '\0') return FALSE;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return TRUE;
}

static bool_t current_is_root(void) {
    sys_user_info_t me;
    if (sys_users_whoami(&me) < 0) return FALSE;
    return me.is_root;
}

static void current_name(char *buf, u32 bufsize) {
    sys_user_info_t me;
    if (sys_users_whoami(&me) >= 0) {
        strncpy(buf, me.name, bufsize - 1);
        buf[bufsize - 1] = '\0';
    } else {
        strncpy(buf, "?", bufsize - 1);
        buf[bufsize - 1] = '\0';
    }
}

/* Ищет имя пользователя по uid через SYS_USERS_LIST -- нет
   специального "по одному uid" syscall'а, но список маленький
   (USERS_MAX, include/users.h), так что перебор каждый раз дёшев.
   Если uid не найден (например, файл принадлежит уже удалённому
   пользователю), возвращает "uid<N>", как и полагается честно
   показать "нет такого", а не выдумывать имя. */
static const char *username_by_uid(u32 uid, char *buf, u32 bufsize) {
    sys_user_info_t users[32];
    u32 count = 0;
    if (sys_users_list(users, 32, &count) >= 0) {
        u32 n = count < 32 ? count : 32;
        for (u32 i = 0; i < n; i++) {
            if (users[i].uid == uid) {
                strncpy(buf, users[i].name, bufsize - 1);
                buf[bufsize - 1] = '\0';
                return buf;
            }
        }
    }
    snprintf(buf, bufsize, "uid%u", uid);
    return buf;
}

static void print_prompt(void) {
    char path[128];
    if (sys_fs_pwd(path, sizeof(path)) < 0) {
        strncpy(path, "?", sizeof(path));
    }
    sys_fs_backend_info_t bi;
    bool_t have_bi = (sys_fs_backend_info(&bi) >= 0);

    char name[SYS_USER_NAME_MAX];
    current_name(name, sizeof(name));
    char sigil = current_is_root() ? '#' : '$';

    if (have_bi && strcmp(bi.name, "RAM") != 0) {
        /* В отличие от старого shell/shell.c, здесь нет номера диска в
           приглашении -- SYS_FS_BACKEND_INFO (include/sysabi.h)
           намеренно не отдаёт индекс смонтированного диска (это
           внутренняя подробность fs/vfs.c, не нужная ради самого
           приглашения) -- честная, маленькая косметическая потеря
           относительно kernel-linked версии. */
        printf("%s@%s:%s:%s%c ", name, OS_NAME, bi.name, path, sigil);
    } else {
        printf("%s@%s:ram:%s%c ", name, OS_NAME, path, sigil);
    }
}

static void cmd_help(void) {
    printf("Available commands:\n");
    printf("  clear             - clear the screen\n");
    printf("  date              - show current date\n");
    printf("  time              - show current time\n");
    printf("  echo <text>       - print text\n");
    printf("  ver               - OS version\n");
    printf("  shutdown          - power off the system (root only)\n");
    printf("  reboot            - reboot the system (root only)\n");
    printf("  drives            - list disk drives\n");
    printf("  lsusb             - list detected USB devices\n");
    printf("  mouse             - show USB mouse buttons/position (if any)\n");
    printf("  select <n>        - mount drive n (FAT32 or UFS, auto-detected), makes it active\n");
    printf("  format <n>        - format drive n; asks which filesystem (FAT32 or UFS) to use\n");
    printf("  ramfs             - switch active filesystem back to RAM (RAMFS)\n");
    printf("  ls                - list files (RAMFS: with rwx perms and owner)\n");
    printf("  cd <dir>          - change directory (.. - up, / - root)\n");
    printf("  mkdir <dir>       - create a directory\n");
    printf("  rm <n>            - remove a file or empty directory\n");
    printf("  write <f> <text>  - write text to a file (overwrite)\n");
    printf("  cat <f>           - show file contents\n");
    printf("  chmod <mode> <f>  - change rwx permissions, octal (e.g. 'chmod 644 f')\n");
    printf("                      RAMFS only; owner or root\n");
    printf("  chown <user> <f>  - change file owner (RAMFS only, root only)\n");
    printf("  stat <f>          - show owner, group and permissions of a file\n");
    printf("  run <bin> [args]  - run a binary as a container, with THIS shell's own\n");
    printf("                      capabilities (see 'help' notes below) -- e.g. 'run\n");
    printf("                      hello', 'run curl -i http://example.com/'\n");
    printf("  runc <manifest> [args] - run a container: manifest picks the exact caps\n");
    printf("  meminfo           - show physical memory and kernel heap usage\n");
    printf("  ps                - list scheduler tasks (see include/task.h)\n");
    printf("  ifconfig          - show network status (interface, IP, mask, gateway, DNS)\n");
    printf("  ifconfig <ip> <netmask> [gw] [dns] - set a static IPv4 address\n");
    printf("  dhcp              - obtain an IPv4 address automatically (DHCP)\n");
    printf("  help              - this help text\n");
    printf("\n");
    printf("User accounts:\n");
    printf("  whoami            - print current username\n");
    printf("  id                - print current uid/username/role\n");
    printf("  users             - list all accounts on the system\n");
    printf("  passwd [name]     - change your password (root: or anyone's)\n");
    printf("  useradd <n> [password] [-r]  - create a user (root only, -r = root user)\n");
    printf("  userdel <n>       - delete a user (root only)\n");
    printf("  su [name]         - switch user (asks password unless you are root)\n");
    printf("  logout            - log out and return to the login screen\n");
    printf("\n");
    printf("This shell is itself a container (Phase 5, step 5.5, see\n");
    printf("CONTAINER_ARCHITECTURE.md): ring 3, syscalls only (include/usyscall.h,\n");
    printf("include/sysabi.h) -- no more trusted than its own manifest allows. 'run'\n");
    printf("and 'runc' spawn children that can never have MORE capabilities than this\n");
    printf("shell has itself (see sys_container_spawn(), kernel/syscall.c).\n");
    printf("\n");
    printf("Retired in this phase (see CONTAINER_ARCHITECTURE.md, Phase 5.5):\n");
    printf("  rungod   - ring-0 execution can no longer be granted by an unprivileged\n");
    printf("             shell -- that would defeat the whole capability model.\n");
    printf("             Binaries that still talk to kapi_t directly (old /bin/calc)\n");
    printf("             can no longer be launched interactively.\n");
    printf("  synctest - exercised kernel-internal task_create()/mutex_t/etc. directly;\n");
    printf("             no ring-3 syscall for spawning raw kernel tasks exists (nor\n");
    printf("             should it).\n");
    printf("  nbtest   - Phase 4.3 debug hook for the netbridge TX queue, superseded\n");
    printf("             by the real rtl8139d driver container (Phase 4.4).\n");
}

static void print_two_digit(u32 v) {
    if (v < 10) printf("0");
    printf("%u", v);
}

static void cmd_date(void) {
    sys_rtc_time_t t;
    if (sys_rtc_read(&t) < 0) { printf("date: failed\n"); return; }
    printf("%u-", t.year);
    print_two_digit(t.month); printf("-");
    print_two_digit(t.day); printf("\n");
}

static void cmd_time(void) {
    sys_rtc_time_t t;
    if (sys_rtc_read(&t) < 0) { printf("time: failed\n"); return; }
    print_two_digit(t.hour); printf(":");
    print_two_digit(t.minute); printf(":");
    print_two_digit(t.second); printf("\n");
}

#define STORAGE_MAX 8
static const char *storage_bus_name(u32 bus) {
    switch (bus) {
        case 0: return "IDE";
        case 1: return "SATA";
        case 2: return "USB";
        default: return "?";
    }
}

static void cmd_drives(void) {
    sys_storage_info_t drives[STORAGE_MAX];
    u32 count = 0;
    if (sys_storage_list(drives, STORAGE_MAX, &count) < 0) {
        printf("drives: failed to query storage\n");
        return;
    }
    if (count == 0) {
        printf("No drives detected.\n");
        return;
    }
    u32 shown = count < STORAGE_MAX ? count : STORAGE_MAX;
    for (u32 i = 0; i < shown; i++) {
        if (!drives[i].present) continue;
        u32 mb = (u32)(((u64)drives[i].sectors * 512) / (1024 * 1024));
        printf("[%u] %s  %u MB  (%s, %s)\n", i, drives[i].model, mb,
               drives[i].location, storage_bus_name(drives[i].bus));
    }
}

#define USB_MAX 8

static void cmd_lsusb(void) {
    sys_usb_info_t devs[USB_MAX];
    u32 count = 0;
    if (sys_usb_list(devs, USB_MAX, &count) < 0) {
        printf("lsusb: failed to query USB\n");
        return;
    }
    if (count == 0) {
        printf("No USB devices detected.\n");
        return;
    }
    u32 shown = count < USB_MAX ? count : USB_MAX;
    for (u32 i = 0; i < shown; i++) {
        if (!devs[i].present) continue;
        printf("[%u] VID:PID %x:%x  class %x/%x/%x  %s-speed\n",
               i, devs[i].vendor_id, devs[i].product_id,
               devs[i].dev_class, devs[i].dev_subclass, devs[i].dev_protocol,
               devs[i].speed == 0 ? "low" : "full");
    }
}

static void cmd_mouse(void) {
    sys_mouse_state_t m;
    if (sys_usb_mouse_state(&m) < 0 || !m.present) {
        printf("No USB mouse detected.\n");
        return;
    }
    printf("USB mouse: buttons=0x%x  x=%d  y=%d\n", m.buttons, m.x, m.y);
}

static void cmd_select(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: select <drive number>\n"); return; }
    u32 idx = (u32)atoi(argv[1]);
    sys_storage_info_t drives[STORAGE_MAX];
    u32 count = 0;
    if (sys_storage_list(drives, STORAGE_MAX, &count) < 0 || idx >= count) {
        printf("No such drive.\n");
        return;
    }
    if (sys_fs_mount(idx) >= 0) {
        sys_fs_backend_info_t bi;
        sys_fs_backend_info(&bi);
        printf("Drive %u mounted (%s).\n", idx, bi.name);
    } else {
        printf("Failed to mount drive %u (unrecognized filesystem, read error, or permission).\n", idx);
    }
}

static bool_t ask_format_type(u32 *out_type) {
    printf("Choose filesystem to format as:\n");
    printf("  [1] FAT32\n");
    printf("  [2] UFS (Unix File System)\n");
    printf("> ");
    char choice[32];
    readline(choice, sizeof(choice));
    trim(choice);

    if (strcmp(choice, "1") == 0 || strcmp(choice, "fat32") == 0 || strcmp(choice, "FAT32") == 0) {
        *out_type = 0; /* FS_TYPE_FAT32, см. комментарий у SYS_FS_FORMAT, include/sysabi.h */
        return TRUE;
    }
    if (strcmp(choice, "2") == 0 || strcmp(choice, "ufs") == 0 || strcmp(choice, "UFS") == 0) {
        *out_type = 1; /* FS_TYPE_UFS */
        return TRUE;
    }
    return FALSE;
}

static void cmd_format(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: format <drive number>\n"); return; }
    u32 idx = (u32)atoi(argv[1]);
    sys_storage_info_t drives[STORAGE_MAX];
    u32 count = 0;
    if (sys_storage_list(drives, STORAGE_MAX, &count) < 0 || idx >= count) {
        printf("No such drive.\n");
        return;
    }

    u32 type;
    if (!ask_format_type(&type)) {
        printf("Unrecognized choice, formatting cancelled.\n");
        return;
    }

    const char *type_name = (type == 1) ? "UFS" : "FAT32";
    printf("Formatting drive %u as %s...\n", idx, type_name);
    if (sys_fs_format(idx, type) >= 0) {
        printf("Done. Drive %u mounted (%s).\n", idx, type_name);
    } else {
        printf("Formatting error (or permission denied).\n");
    }
}

static void cmd_meminfo(void) {
    sys_meminfo_t info;
    if (sys_meminfo(&info) < 0) { printf("meminfo: failed\n"); return; }
    u32 total_kb = info.mem_total_bytes / 1024;
    u32 free_kb  = info.mem_free_bytes / 1024;
    u32 used_kb  = total_kb - free_kb;
    u32 heap_total_kb = info.heap_total_bytes / 1024;
    u32 heap_used_kb  = info.heap_used_bytes / 1024;

    printf("Physical memory: %u KB total, %u KB used, %u KB free.\n", total_kb, used_kb, free_kb);
    printf("Kernel heap:     %u KB total, %u KB used, %u KB free.\n",
           heap_total_kb, heap_used_kb, heap_total_kb - heap_used_kb);
    printf("Note: this is the kernel's own heap -- your own printf()/malloc() here\n");
    printf("use a completely separate, private heap (see include/libc.h).\n");
}

static void cmd_ifconfig(u32 argc, char **argv) {
    if (argc == 1) {
        sys_net_status_t cfg;
        if (sys_net_status(&cfg) < 0 || !cfg.up) {
            printf("net0: no network card found.\n");
            return;
        }
        printf("net0: up, mac=%x:%x:%x:%x:%x:%x\n",
               cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5]);
        printf("      inet "); print_ip(cfg.ip);
        printf("  netmask "); print_ip(cfg.netmask);
        printf("  gateway "); print_ip(cfg.gateway);
        printf("\n      dns "); print_ip(cfg.dns);
        printf(cfg.dhcp_bound ? "  (via DHCP)\n" : "  (static)\n");
        return;
    }

    if (argc < 3) {
        printf("usage: ifconfig                              -- show current configuration\n");
        printf("       ifconfig <ip> <netmask> [gateway] [dns] -- set a static address\n");
        printf("       dhcp                                    -- obtain an address via DHCP\n");
        return;
    }

    u32 ip, mask, gw = 0, dns = 0;
    if (!parse_ipv4(argv[1], &ip) || !parse_ipv4(argv[2], &mask)) {
        printf("ifconfig: invalid IPv4 address\n");
        return;
    }
    if (argc >= 4 && !parse_ipv4(argv[3], &gw)) {
        printf("ifconfig: invalid gateway address\n");
        return;
    }
    if (argc >= 5 && !parse_ipv4(argv[4], &dns)) {
        printf("ifconfig: invalid DNS address\n");
        return;
    }

    if (sys_net_set_static(ip, mask, gw, dns) >= 0) {
        printf("net0: configured.\n");
    } else {
        printf("ifconfig: permission denied\n");
    }
}

static void cmd_dhcp(void) {
    printf("dhcp: requesting address...\n");
    if (sys_net_dhcp(8000) < 0) {
        printf("dhcp: no response (timed out), no network card, or permission denied.\n");
        return;
    }
    sys_net_status_t cfg;
    sys_net_status(&cfg);
    printf("dhcp: bound "); print_ip(cfg.ip);
    printf(" (netmask "); print_ip(cfg.netmask);
    printf(", gateway "); print_ip(cfg.gateway);
    printf(", dns "); print_ip(cfg.dns);
    printf(")\n");
}

static const char *task_state_name(u32 s) {
    switch (s) {
        case 0: return "ready";
        case 1: return "running";
        case 2: return "blocked";
        case 3: return "terminated";
        default: return "?";
    }
}

#define PS_MAX 64

static void cmd_ps(void) {
    sys_task_info_t tasks[PS_MAX];
    u32 count = 0;
    if (sys_ps_list(tasks, PS_MAX, &count) < 0) {
        printf("ps: permission denied or failed\n");
        return;
    }
    printf("PID  STATE       NAME\n");
    u32 shown = count < PS_MAX ? count : PS_MAX;
    for (u32 i = 0; i < shown; i++) {
        printf("%u", tasks[i].id);
        printf("    %s", task_state_name(tasks[i].state));
        printf("    %s\n", tasks[i].name);
    }
    printf("\n%u task(s) total (showing up to %u). Preemptive scheduler:\n", count, PS_MAX);
    printf("IDT + PIT timer + context switch (see include/task.h).\n");
}

static void cmd_echo(const char *raw_args) {
    printf("%s\n", raw_args);
}

static void cmd_whoami(void) {
    char name[SYS_USER_NAME_MAX];
    current_name(name, sizeof(name));
    printf("%s\n", name);
}

static void cmd_id(void) {
    sys_user_info_t me;
    if (sys_users_whoami(&me) < 0) { printf("id: failed\n"); return; }
    printf("uid=%u(%s) group=%s\n", me.uid, me.name, me.is_root ? "root" : "users");
}

#define USERS_LIST_MAX 32

static void cmd_users(void) {
    sys_user_info_t users[USERS_LIST_MAX];
    u32 count = 0;
    if (sys_users_list(users, USERS_LIST_MAX, &count) < 0) {
        printf("users: failed\n");
        return;
    }
    printf("UID  NAME             ROLE\n");
    u32 shown = count < USERS_LIST_MAX ? count : USERS_LIST_MAX;
    for (u32 i = 0; i < shown; i++) {
        printf("%u", users[i].uid);
        printf("    %s", users[i].name);
        printf("    %s\n", users[i].is_root ? "root" : "user");
    }
    printf("\n%u account(s) total.\n", count);
}

static void cmd_passwd(u32 argc, char **argv) {
    char me_name[SYS_USER_NAME_MAX];
    current_name(me_name, sizeof(me_name));
    const char *target = (argc >= 2) ? argv[1] : me_name;
    bool_t is_self = (strcmp(target, me_name) == 0);

    char current[64];
    const char *current_ptr = 0;
    if (is_self && !current_is_root()) {
        printf("Current password: ");
        sys_users_read_password(current, sizeof(current));
        current_ptr = current;
    }

    char newpass[64], confirm[64];
    printf("New password: ");
    sys_users_read_password(newpass, sizeof(newpass));
    printf("Confirm password: ");
    sys_users_read_password(confirm, sizeof(confirm));
    if (strcmp(newpass, confirm) != 0) { printf("Passwords do not match.\n"); return; }
    if (newpass[0] == '\0') { printf("Password cannot be empty.\n"); return; }

    if (sys_users_passwd(target, current_ptr, newpass) >= 0) {
        printf("Password updated for %s.\n", target);
    } else {
        printf("passwd: failed (no such user, permission denied, or authentication failure)\n");
    }
}

static void cmd_useradd(u32 argc, char **argv) {
    if (argc < 2) {
        printf("Usage: useradd <n> [password] [-r]\n");
        printf("  -r  create the user as a root (administrator) account\n");
        return;
    }

    const char *name = argv[1];
    bool_t make_root = FALSE;
    const char *password_arg = 0;
    for (u32 i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root") == 0) {
            make_root = TRUE;
        } else if (!password_arg) {
            password_arg = argv[i];
        }
    }

    char password[64];
    if (password_arg) {
        strncpy(password, password_arg, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    } else {
        char confirm[64];
        printf("New password: ");
        sys_users_read_password(password, sizeof(password));
        printf("Confirm password: ");
        sys_users_read_password(confirm, sizeof(confirm));
        if (strcmp(password, confirm) != 0) { printf("Passwords do not match.\n"); return; }
        if (password[0] == '\0') { printf("Password cannot be empty.\n"); return; }
    }

    if (sys_users_add(name, password, make_root) >= 0) {
        printf("User '%s' created%s.\n", name, make_root ? " (root)" : "");
    } else {
        printf("useradd: failed (permission denied, invalid/duplicate name, bad password, or table full)\n");
    }
}

static void cmd_userdel(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: userdel <n>\n"); return; }
    if (sys_users_del(argv[1]) >= 0) {
        printf("User '%s' deleted.\n", argv[1]);
    } else {
        printf("userdel: failed (no such user, permission denied, currently logged in, or last root account)\n");
    }
}

static void cmd_su(u32 argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : "root";
    char pass[64];
    const char *pass_ptr = 0;
    if (!current_is_root()) {
        printf("Password: ");
        sys_users_read_password(pass, sizeof(pass));
        pass_ptr = pass;
    }
    if (sys_users_su(target, pass_ptr) >= 0) {
        printf("Switched to user '%s'.\n", target);
    } else {
        printf("su: failed (no such user or authentication failure)\n");
    }
}

static void cmd_logout(void) {
    char name[SYS_USER_NAME_MAX];
    current_name(name, sizeof(name));
    printf("Logging out %s...\n", name);
    /* Блокирует прямо в ядре, в обход нашего собственного stdio, пока
       кто-то не введёт верную пару логин/пароль -- см. комментарий у
       SYS_USERS_LOGOUT, include/sysabi.h. */
    sys_users_logout();
}

/* ---- run/runc -- см. общий комментарий про 'run' в начале файла ---- */

static void report_exit(const char *label, s32 rc) {
    if (rc < 0) {
        printf("[%s: failed to start (error %d) -- not found, not executable, or permission denied]\n", label, rc);
    } else if (rc != 0) {
        printf("[%s exited with code %d]\n", label, rc);
    }
}

static void cmd_run(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: run <bin> [args...]\n"); return; }
    const char *bin = argv[1];

    char manifest_text[192];
    s32 n = snprintf(manifest_text, sizeof(manifest_text),
                      "name=run\npath=%s\ncaps=" SHELL_CHILD_CAPS "\n", bin);
    if (n < 0 || (u32)n >= sizeof(manifest_text)) {
        printf("run: binary path too long\n");
        return;
    }
    if (sys_fs_write_file(RUN_MANIFEST_PATH, manifest_text, (u32)n) < 0) {
        printf("run: could not prepare manifest (write failed)\n");
        return;
    }

    /* argv[0]="run", argv[1]=bin -- ребёнку передаём только "хвост"
       (argv[2..]), argv[0] внутри контейнера ядро само подставит из
       манифеста (manifest.path) -- см. общий комментарий у
       SYS_CONTAINER_SPAWN, include/sysabi.h. */
    s32 rc = sys_container_spawn(RUN_MANIFEST_PATH, argc - 2, &argv[2]);
    report_exit(bin, rc);
}

static void cmd_runc(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: runc <manifest> [args...]\n"); return; }
    s32 rc = sys_container_spawn(argv[1], argc - 2, &argv[2]);
    report_exit(argv[1], rc);
}

/* ---- файловые команды ---- */

static bool_t parse_octal_mode(const char *s, u32 *out) {
    if (!s || s[0] == '\0') return FALSE;
    u32 len = strlen(s);
    if (len > 4) return FALSE;
    u32 val = 0;
    for (u32 i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '7') return FALSE;
        val = val * 8 + (u32)(c - '0');
    }
    *out = val & 0x1FFu; /* 9 бит rwxrwxrwx -- тот же смысл, что RAMFS_MODE_MASK (fs/ramfs.c) */
    return TRUE;
}

static void cmd_chmod(u32 argc, char **argv) {
    if (argc < 3) { printf("Usage: chmod <mode> <file>\n"); return; }
    u32 mode;
    if (!parse_octal_mode(argv[1], &mode)) {
        printf("chmod: invalid mode '%s' (use octal, e.g. 755 or 644)\n", argv[1]);
        return;
    }
    if (sys_fs_chmod(argv[2], mode) < 0) {
        printf("chmod: cannot change permissions of '%s' (not found, no permission, or RAMFS only)\n", argv[2]);
    }
}

static void cmd_chown(u32 argc, char **argv) {
    if (argc < 3) { printf("Usage: chown <user> <file>\n"); return; }

    sys_user_info_t users[USERS_LIST_MAX];
    u32 count = 0;
    u32 uid = 0;
    bool_t found = FALSE;
    if (sys_users_list(users, USERS_LIST_MAX, &count) >= 0) {
        u32 n = count < USERS_LIST_MAX ? count : USERS_LIST_MAX;
        for (u32 i = 0; i < n; i++) {
            if (strcmp(users[i].name, argv[1]) == 0) { uid = users[i].uid; found = TRUE; break; }
        }
    }
    if (!found) { printf("chown: no such user: %s\n", argv[1]); return; }

    if (sys_fs_chown(argv[2], uid) < 0) {
        printf("chown: cannot change owner of '%s' (not found, not root, or RAMFS only)\n", argv[2]);
    }
}

static void cmd_stat(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: stat <file>\n"); return; }
    sys_fs_stat_t info;
    if (sys_fs_stat(argv[1], &info) < 0) {
        printf("stat: %s: not found (or RAMFS only)\n", argv[1]);
        return;
    }
    char owner[SYS_USER_NAME_MAX];
    username_by_uid(info.owner_uid, owner, sizeof(owner));
    printf("%s\n", argv[1]);
    printf("  owner: %s (uid %u)\n", owner, info.owner_uid);
    printf("  group: %s\n", info.owner_gid == 0 ? "root" : "users");
    printf("  mode:  %u%u%u (rwx: owner/group/other)\n",
           (info.mode >> 6) & 07u, (info.mode >> 3) & 07u, info.mode & 07u);
}

static void cmd_write(u32 argc, char **argv, const char *raw_line) {
    if (argc < 2) { printf("Usage: write <file> <text>\n"); return; }
    const char *p = raw_line;
    while (*p && *p != ' ' && *p != '\t') p++; /* пропускаем "write" */
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t') p++; /* пропускаем имя файла */
    while (*p == ' ' || *p == '\t') p++;

    if (sys_fs_write_file(argv[1], p, strlen(p)) < 0) {
        printf("File write error.\n");
    }
}

#define CAT_BUF_SIZE 8192
static char g_cat_buf[CAT_BUF_SIZE];

static void cmd_cat(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: cat <file>\n"); return; }
    u32 size = 0;
    if (sys_fs_read_file(argv[1], g_cat_buf, CAT_BUF_SIZE - 1, &size) < 0) {
        printf("Failed to read file: %s\n", argv[1]);
        return;
    }
    g_cat_buf[size] = '\0';
    printf("%s\n", g_cat_buf);
}

static void mode_to_str(bool_t is_dir, u32 mode, char *out) {
    out[0] = is_dir ? 'd' : '-';
    out[1] = (mode & 0400u) ? 'r' : '-';
    out[2] = (mode & 0200u) ? 'w' : '-';
    out[3] = (mode & 0100u) ? 'x' : '-';
    out[4] = (mode & 0040u) ? 'r' : '-';
    out[5] = (mode & 0020u) ? 'w' : '-';
    out[6] = (mode & 0010u) ? 'x' : '-';
    out[7] = (mode & 0004u) ? 'r' : '-';
    out[8] = (mode & 0002u) ? 'w' : '-';
    out[9] = (mode & 0001u) ? 'x' : '-';
    out[10] = '\0';
}

#define LS_MAX_ENTRIES 64
static sys_fs_dirent_t g_ls_entries[LS_MAX_ENTRIES];

static void cmd_ls(void) {
    u32 count = 0;
    if (sys_fs_list(g_ls_entries, LS_MAX_ENTRIES, &count) < 0) {
        printf("ls: failed to list directory\n");
        return;
    }
    if (count == 0) { printf("  (empty)\n"); return; }

    u32 shown = count < LS_MAX_ENTRIES ? count : LS_MAX_ENTRIES;
    for (u32 i = 0; i < shown; i++) {
        char perm[11];
        mode_to_str(g_ls_entries[i].is_dir, g_ls_entries[i].mode, perm);
        char owner[SYS_USER_NAME_MAX];
        username_by_uid(g_ls_entries[i].owner_uid, owner, sizeof(owner));
        if (g_ls_entries[i].is_dir) {
            printf("  %s  %s\t<DIR>\t%s\n", perm, owner, g_ls_entries[i].name);
        } else if (g_ls_entries[i].is_app) {
            printf("  %s  %s\t%u\t%s\t[app]\n", perm, owner, g_ls_entries[i].size, g_ls_entries[i].name);
        } else {
            printf("  %s  %s\t%u\t%s\n", perm, owner, g_ls_entries[i].size, g_ls_entries[i].name);
        }
    }
    if (count > LS_MAX_ENTRIES) {
        printf("  ... and %u more (see LS_MAX_ENTRIES in apps/shell.c)\n", count - LS_MAX_ENTRIES);
    }
}

static void cmd_cd(u32 argc, char **argv) {
    if (argc < 2) { printf("Usage: cd <directory>\n"); return; }
    if (sys_fs_cd(argv[1]) < 0) { printf("Directory not found: %s\n", argv[1]); }
}

/* ---- главный цикл ---- */

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    char line[MAX_LINE];
    char line_copy[MAX_LINE];
    char *sargv[MAX_ARGS];

    printf(OS_NAME " v" OS_VERSION " -- a text-mode OS with FAT32 and UFS support\n");
    printf("Running as a container (Phase 5.5, see CONTAINER_ARCHITECTURE.md) -- ring 3, syscalls only.\n");
    printf("Type 'help' for a list of commands.\n\n");

    for (;;) {
        print_prompt();
        s32 n = readline(line, MAX_LINE);
        if (n < 0) { sys_yield(); continue; }
        trim(line);
        if (line[0] == '\0') continue;

        strncpy(line_copy, line, MAX_LINE);
        line_copy[MAX_LINE - 1] = '\0';
        u32 sargc = split_ws(line_copy, sargv, MAX_ARGS);
        if (sargc == 0) continue;

        if (strcmp(sargv[0], "clear") == 0) {
            sys_clear();
        } else if (strcmp(sargv[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(sargv[0], "ver") == 0) {
            printf("%s version %s\n", OS_NAME, OS_VERSION);
        } else if (strcmp(sargv[0], "date") == 0) {
            cmd_date();
        } else if (strcmp(sargv[0], "time") == 0) {
            cmd_time();
        } else if (strcmp(sargv[0], "echo") == 0) {
            const char *p = line;
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            cmd_echo(p);
        } else if (strcmp(sargv[0], "shutdown") == 0) {
            if (sys_power_shutdown() < 0) printf("shutdown: permission denied (root only)\n");
        } else if (strcmp(sargv[0], "reboot") == 0) {
            if (sys_power_reboot() < 0) printf("reboot: permission denied (root only)\n");
        } else if (strcmp(sargv[0], "drives") == 0) {
            cmd_drives();
        } else if (strcmp(sargv[0], "lsusb") == 0) {
            cmd_lsusb();
        } else if (strcmp(sargv[0], "mouse") == 0) {
            cmd_mouse();
        } else if (strcmp(sargv[0], "select") == 0) {
            cmd_select(sargc, sargv);
        } else if (strcmp(sargv[0], "format") == 0) {
            cmd_format(sargc, sargv);
        } else if (strcmp(sargv[0], "ramfs") == 0) {
            if (sys_fs_use_ramfs() >= 0) printf("Active filesystem: RAMFS.\n");
            else printf("ramfs: permission denied\n");
        } else if (strcmp(sargv[0], "ifconfig") == 0) {
            cmd_ifconfig(sargc, sargv);
        } else if (strcmp(sargv[0], "dhcp") == 0) {
            cmd_dhcp();
        } else if (strcmp(sargv[0], "run") == 0) {
            cmd_run(sargc, sargv);
        } else if (strcmp(sargv[0], "runc") == 0) {
            cmd_runc(sargc, sargv);
        } else if (strcmp(sargv[0], "meminfo") == 0) {
            cmd_meminfo();
        } else if (strcmp(sargv[0], "ps") == 0) {
            cmd_ps();
        } else if (strcmp(sargv[0], "ls") == 0) {
            cmd_ls();
        } else if (strcmp(sargv[0], "cd") == 0) {
            cmd_cd(sargc, sargv);
        } else if (strcmp(sargv[0], "mkdir") == 0) {
            if (sargc < 2) { printf("Usage: mkdir <n>\n"); }
            else if (sys_fs_mkdir(sargv[1]) < 0) { printf("Failed to create directory (or permission denied).\n"); }
        } else if (strcmp(sargv[0], "rm") == 0) {
            if (sargc < 2) { printf("Usage: rm <n>\n"); }
            else if (sys_fs_rm(sargv[1]) < 0) { printf("Failed to remove: %s (or permission denied)\n", sargv[1]); }
        } else if (strcmp(sargv[0], "cat") == 0) {
            cmd_cat(sargc, sargv);
        } else if (strcmp(sargv[0], "write") == 0) {
            cmd_write(sargc, sargv, line);
        } else if (strcmp(sargv[0], "chmod") == 0) {
            cmd_chmod(sargc, sargv);
        } else if (strcmp(sargv[0], "chown") == 0) {
            cmd_chown(sargc, sargv);
        } else if (strcmp(sargv[0], "stat") == 0) {
            cmd_stat(sargc, sargv);
        } else if (strcmp(sargv[0], "whoami") == 0) {
            cmd_whoami();
        } else if (strcmp(sargv[0], "id") == 0) {
            cmd_id();
        } else if (strcmp(sargv[0], "users") == 0) {
            cmd_users();
        } else if (strcmp(sargv[0], "passwd") == 0) {
            cmd_passwd(sargc, sargv);
        } else if (strcmp(sargv[0], "useradd") == 0) {
            cmd_useradd(sargc, sargv);
        } else if (strcmp(sargv[0], "userdel") == 0) {
            cmd_userdel(sargc, sargv);
        } else if (strcmp(sargv[0], "su") == 0) {
            cmd_su(sargc, sargv);
        } else if (strcmp(sargv[0], "logout") == 0) {
            cmd_logout();
        } else {
            printf("Unknown command: %s (type 'help')\n", sargv[0]);
        }
    }

    return 0; /* недостижимо -- цикл выше бесконечен */
}

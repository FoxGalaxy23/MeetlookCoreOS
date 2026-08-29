#include "vga.h"
#include "keyboard.h"
#include "storage.h"
#include "usb.h"
#include "vfs.h"
#include "container.h"
#include "string.h"
#include "kprintf.h"
#include "types.h"
#include "bootinfo.h"
#include "pmm.h"
#include "kheap.h"
#include "idt.h"
#include "isr.h"
#include "irqbridge.h"
#include "pic.h"
#include "paging.h"
#include "gdt.h"
#include "syscall.h"
#include "timer.h"
#include "task.h"
#include "users.h"
#include "net.h"
#include "netbridge.h"
#include "netdrv.h"
#include "loader.h"
#include "percpu.h"
#include "smp.h"
#include "bootconfirm.h"

/* ---- Демонстрационные фоновые задачи ----
 * Показывают, что планировщик действительно вытесняет: каждая просто
 * крутится в бесконечном цикле и считает свои "тики", по одному за
 * каждый раз, когда снова получает процессор (см. task_yield ниже) --
 * значения видны через команду шелла 'ps'. Пока пользователь работает
 * в шелле (в т.ч. когда keyboard_getchar() крутится в busy-wait, ожидая
 * нажатия клавиши), таймер продолжает прерывать эту "работу" и отдавать
 * время этим задачам -- то есть счётчики растут даже без вызова 'ps'. */
static volatile u64 g_demo_counter_a = 0;
static volatile u64 g_demo_counter_b = 0;

static void demo_task_a(void *arg) {
    (void)arg;
    for (;;) {
        g_demo_counter_a++;
        task_yield();
    }
}

static void demo_task_b(void *arg) {
    (void)arg;
    for (;;) {
        g_demo_counter_b++;
        task_yield();
    }
}

u64 demo_counter_a(void) { return g_demo_counter_a; }
u64 demo_counter_b(void) { return g_demo_counter_b; }

/* ---- Фаза 5, подшаг 5.5 ----
 * loader_run_tick_services() (kernel/loader.c, регистрация -- см.
 * kapi->register_tick, include/kapi.h) раньше вызывалась прямо из
 * цикла shell_run() (shell/shell.c) -- ей это подходило, т.к. сама
 * shell_run() была кодом ядра. Теперь, когда шелл -- обычный ring-3
 * контейнер (apps/shell.c) без доступа к функциям ядра, этот крючок
 * (сейчас им никто не пользуется -- ни один rungod-бинарник в дереве
 * не зовёт register_tick(), но сам механизм остаётся частью kapi_t
 * ради будущих ring-0 приложений) обязан кто-то продолжать дёргать --
 * иначе он тихо перестал бы работать без единого сообщения об ошибке
 * где-либо. Отдельный поток ядра, ровно как netbridge-rx ниже, только
 * с фиксированным интервалом, а не блокировкой на семафоре. */
static void tick_services_task(void *arg) {
    (void)arg;
    for (;;) {
        loader_run_tick_services();
        task_sleep(10);
    }
}

void kernel_main(const boot_info_t *bi) {
    /* SMP: this_cpu() (include/percpu.h) обязан работать УЖЕ с самой
       первой строчки, где он может понадобиться -- в частности,
       gdt_init() ниже читает this_cpu()->cpu_index, чтобы понять, чей
       персональный GDT/TSS готовить (см. kernel/gdt.c). lapic_id пока
       заведомо неточный (0) -- LAPIC ещё не инициализирован, настоящий
       физический APIC ID BSP появится только внутри smp_init() (см.
       kernel/smp.c), который сам поправит this_cpu()->lapic_id. cpu_index
       BSP всегда 0 -- это соглашение всей SMP-подсистемы (см. include/smp.h). */
    percpu_init_early(0, 0);

    vga_init();
    keyboard_init();

    pmm_init(bi);
    kheap_init();
    kprintf("Memory: %u KB total, %u KB free physical, %u KB kernel heap.\n",
            pmm_total_memory_bytes() / 1024,
            pmm_free_memory_bytes() / 1024,
            kheap_total_bytes() / 1024);

    /* ATOMIC_PLAN.md, Фаза 6 -- см. bootconfirm_init(), kernel/bootconfirm.c.
     * Максимально рано (хоть и не обязательно раньше pmm_init() выше --
     * просто удобное соседство по месту в файле, оба читают bi) --
     * запоминает нужные поля bi ДО того, как что-либо ещё в kernel_main()
     * теоретически могло бы усложнить жизнь, если понадобится вызвать
     * bootconfirm_mark_ok() раньше обычного места (после логина, ниже
     * по этому же файлу). */
    bootconfirm_init(bi);

    /* ---- GDT + TSS + IDT + PIC + таймер + планировщик задач ----
     * Порядок важен: сначала своя GDT с дескрипторами ring0/ring3 и TSS
     * (gdt_init) -- ДО idt_init, т.к. дальше используется тот же
     * KERNEL_CODE_SEL для гейтов, а PIT/клавиатура ещё не должны успеть
     * прервать выполнение раньше времени. Потом таблица прерываний
     * (idt_init) и регистрация вектора syscall'ов (syscall_init) --
     * см. include/gdt.h, include/syscall.h. Дальше планировщик получает
     * "задачу 0" (tasking_init -- ею становится сам этот поток
     * исполнения), затем можно создавать дополнительные задачи, и
     * только в конце -- запустить таймер (timer_init) и разрешить
     * прерывания (sti). До sti система ведёт себя ровно как раньше
     * (однозадачно), и это безопасно, т.к. tasking_init() ничего не
     * переключает сама по себе -- переключения запускает только
     * таймерный тик через scheduler_tick(). */
    gdt_init();
    idt_init();
    isr_init();
    irqbridge_init(); /* см. include/irqbridge.h -- до pic_remap()/sti, семафоры готовы заранее */
    pic_remap(PIC1_VECTOR_OFFSET, PIC2_VECTOR_OFFSET); /* сдвигаем IRQ0-15 на векторы 32-47,
        иначе они попадают на исключения CPU (IRQ0-7) или вообще на пустые гейты IDT
        (IRQ8-15 -> 0x70-0x77 по дефолту от BIOS), что и вызывало #GP при первом же
        аппаратном прерывании выше вектора 47 (например, IRQ14 от ATA-диска). */
    paging_init();
    syscall_init();
    tasking_init();
    task_create("demo-a", demo_task_a, 0);
    task_create("demo-b", demo_task_b, 0);
    task_create("tick-services", tick_services_task, 0);
    timer_init(100); /* 100 Гц: каждая задача получает ~50 мс (см. TASK_TIME_SLICE_TICKS) */
    __asm__ volatile ("sti");
    kprintf("Scheduler online: IDT + PIT timer (100 Hz) + preemptive context switching.\n");
    kprintf("Use 'ps' to see running tasks.\n");

    /* SMP (см. include/smp.h за общей картиной): обнаруживает через ACPI
       остальные ядра машины и поднимает их одно за другим, каждое --
       со своим собственным GDT/TSS/percpu и локальным LAPIC-таймером,
       участвующим в ТОЙ ЖЕ общей очереди планировщика, что и BSP. Вызов
       обязан идти ПОСЛЕ tasking_init() (нужен хотя бы один узел общего
       кольца, к которому подключатся AP, см. task_register_this_cpu())
       и ПОСЛЕ timer_init() (нужна откалиброванная частота PIT, чтобы
       откалибровать по ней локальные таймеры APIC, см.
       lapic_timer_calibrate() в kernel/apic.c). Если ACPI недоступен
       или в системе физически одно ядро, тихо продолжает работу
       однопроцессорно -- см. общий комментарий в kernel/smp.c. */
    smp_init();
    kprintf("CPUs online: %u.\n", smp_cpu_count());

    storage_init(); /* обнаруживает диски и на IDE, и на SATA (AHCI) */
    usb_init(); /* контроллер UHCI: клавиатура/мышь (HID) + флеш-накопители
                   (Mass Storage, добавляются в тот же реестр storage.h,
                   поэтому storage_init() должен отработать раньше) */

    net_init(); /* обнаруживает сетевую карту (RTL8139) на шине PCI и
        программирует её DMA-буферы (netdrv_probe(), kernel/netdrv.c),
        поднимает Ethernet/ARP/IP/ICMP/UDP/TCP -- см. include/net.h. Сам
        драйвер-контейнер (apps/rtl8139d.c) здесь ЕЩЁ не запущен -- см.
        netdrv_launch() ниже, после vfs_init(). IP не настраивается сама
        по себе: 'ifconfig' (статически) или 'dhcp' (через QEMU slirp) в
        шелле, либо SYS_NET_DHCP/... из песочницы. */

    netbridge_init(); /* Фаза 4, подшаг 3 (см. include/netbridge.h) -- канал
        кадров ядро<->драйвер-контейнер. Заводится независимо от того,
        нашлась ли физически карта (net_is_up()): нужен и для настоящего
        rtl8139d (подшаг 4, запускается ниже, после vfs_init()), и сам по
        себе пригоден для демонстрационного apps/netdrv_echo.c. */
    task_create("netbridge-rx", netbridge_rx_pump, 0); /* блокирующе ждёт
        кадры от RX-очереди и передаёт их в eth_input() -- отдельный
        поток ядра, а не что-то, разбуженное таймером/IRQ напрямую, ровно
        как demo-a/demo-b выше, только с настоящей работой вместо счётчика */

    kprintf("Hardware initialization complete.\n");
    kprintf("Drives detected: %u\n", storage_drive_count());
    if (storage_drive_count() > 0) {
        kprintf("Hint: use 'drives' and 'select <n>' to work with FAT32.\n");
    }

    vfs_init();
    kprintf("RAMFS mounted as root ('/'), base files loaded (readme.txt, /bin/calc, /bin/hello).\n");
    kprintf("'run <name>' (inside the shell) executes a binary as a container: it can only\n");
    kprintf("touch its own memory and must use syscalls (int 0x80) for everything else.\n");
    kprintf("Note (Phase 5.5, see CONTAINER_ARCHITECTURE.md): the shell itself is now such\n");
    kprintf("a container, so it can no longer grant ring-0 access to anything it starts --\n");
    kprintf("/bin/calc, which still talks to kapi_t directly, can no longer be launched\n");
    kprintf("interactively for exactly that reason.\n");
    kprintf("\n");

    netdrv_launch(); /* Фаза 4, подшаг 4 (см. include/netdrv.h) -- запускает
        /bin/rtl8139d как контейнер-драйвер, если net_init() выше нашёл
        карту. Обязан идти ПОСЛЕ vfs_init() (container_spawn() внутри
        читает ELF из RAMFS через fs_get_app(), см. kernel/container.c) и
        ДО запуска шелл-контейнера ниже (сам не блокирует -- заводит
        отдельный поток ядра, см. kernel/netdrv.c, но kernel_main() всё
        равно должен успеть вызвать его раньше, чем уйдёт в
        container_spawn() шелла и не вернётся). */

    /* ---- Система пользователей ----
     * Создаёт учётные записи root/user (uid 0 / uid 1) и требует входа
     * перед запуском шелла, как в обычной ОС. См. include/users.h и
     * kernel/users.c; команды useradd/userdel/passwd/su/whoami/id/users
     * реализованы в shell/shell.c поверх этого API. */
    users_init();
    kprintf("User accounts ready: 'root' (root:root) and 'user' (user:user).\n");
    users_login_prompt();

    /* ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у bootconfirm_mark_ok(),
     * kernel/bootconfirm.c: "дошли до рабочего состояния" -- ФС смонтирована
     * (RAMFS, см. выше по файлу) И вход в систему успешен (строка прямо
     * над этой -- users_login_prompt() возвращается только после
     * успешного логина, см. её же комментарий, kernel/users.c). Тихо
     * ничего не делает на обычной (не-Atomic) загрузке -- см. её
     * комментарий, это подавляющее большинство случаев сегодня, не
     * ошибка. */
    bootconfirm_mark_ok();

    /* ---- Фаза 5, подшаг 5.5: шелл как контейнер (см. CONTAINER_ARCHITECTURE.md) ----
     * До этой строки shell_run() (shell/shell.c) была слинкована прямо в
     * kernel.bin и звала функции ядра напрямую. Теперь это обычный
     * ring-3 бинарник, apps/shell.c -> /bin/shell, и запускается ровно
     * тем же container_spawn(), что и netdrv_launch() выше -- разница
     * только в том, что здесь мы блокируемся на нём (см. возврат
     * container_spawn(), не оборачиваем в task_create()), т.к. кроме
     * него (и уже запущенных фоновых потоков) kernel_main() всё равно
     * больше ничего не делает.
     *
     * caps -- объединение ВСЕХ capability, которые появились в
     * подшагах 5.1-5.4 (CAP_FS_READ/CAP_PS/CAP_USERS/CAP_POWER/
     * CAP_SYSINFO) плюс то, что было нужно шеллу и раньше
     * (CAP_NET/CAP_FS_WRITE/CAP_SHM/CAP_SPAWN) -- де-факто "всё, кроме
     * CAP_IO/CAP_IRQ/CAP_NET_BRIDGE": эти три нужны ТОЛЬКО
     * драйверам-контейнерам (rtl8139d), пользующимся сырым железом или
     * сырыми Ethernet-кадрами напрямую, а не интерактивному шеллу. См.
     * apps/shell.c (макрос SHELL_CHILD_CAPS) -- 'run' внутри шелла
     * порождает детей РОВНО с этим же набором прав (см. общий
     * комментарий там), поэтому оба места обязаны совпадать. */
    container_manifest_t shell_manifest;
    memset(&shell_manifest, 0, sizeof(shell_manifest));
    strncpy(shell_manifest.name, "shell", CONTAINER_NAME_MAX - 1);
    strncpy(shell_manifest.path, "/bin/shell", CONTAINER_PATH_MAX - 1);
    shell_manifest.caps = CAP_NET | CAP_FS_WRITE | CAP_FS_READ | CAP_SHM |
                           CAP_SPAWN | CAP_PS | CAP_USERS | CAP_POWER | CAP_SYSINFO;
    shell_manifest.irq_line = CONTAINER_IRQ_NONE;

    char *shell_argv[1];
    shell_argv[0] = shell_manifest.path;
    s32 shell_rc = container_spawn(&shell_manifest, 1, shell_argv);

    /* Шелл сам по себе -- бесконечный цикл (см. apps/shell.c), сюда
       попадаем, только если он упал (авария в ring 3 либо явный
       sys_exit(), см. kernel/isr.c) -- ни автоперезапуска, ни возврата
       к shell_run() тут больше нет и быть не может (см. общий
       комментарий в netdrv_launch_task() выше про тот же принцип). */
    kprintf("shell: container exited (rc=%d) -- system halted, reboot required\n", shell_rc);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

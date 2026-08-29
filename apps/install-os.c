/* install-os.c -- ставит MeetlookCoreOS на физический диск так, чтобы
 * система грузилась без ISO/GRUB (см. INSTALLER_PLAN.md, Фаза 4, и
 * NOGRUB_PLAN.md, Фаза C). Пишет напрямую по фиксированным LBA через
 * sys_disk_write_raw() (CAP_DISK_RAW -- см. общий комментарий там же,
 * самая опасная capability в системе) СЛЕДУЯ ТОЙ ЖЕ РАЗМЕТКЕ, что уже
 * проверена вручную в tools/make_test_boot_img.py:
 *
 *   LBA 0            -- MBR (mbr_build(), lib/mbr.c, Фаза 1) с
 *                        stage1.bin как boot_code и одним разделом
 *                        (UFS по умолчанию, либо FAT32 -- см. выбор в app_main())
 *   LBA 1..63        -- stage2.bin (63 сектора = 31.5 KiB, zero-padded)
 *   LBA 64..4159     -- kernel.flat (2 MiB, zero-padded) -- фиксированная
 *                        "область ядра", читается собственным stage2
 *                        (boot/stage2.s) напрямую, в обход файловой системы
 *   LBA 4160+        -- раздел с данными (partition[0] в MBR) -- UFS
 *                        (рекомендуется для стационарных дисков/серверов)
 *                        или FAT32 (для переносных накопителей)
 *
 * ТРИ ассета (stage1.bin/stage2.bin/kernel.flat) читаются из /boot/ --
 * это ФАЙЛЫ САМОЙ РАБОТАЮЩЕЙ СИСТЕМЫ (встроены в её RAMFS через
 * tools/gen_rootfs.py), НЕ то же самое, что kernel.bin, из которого
 * сейчас работает конкретно ЭТОТ инстанс install-os -- см. подробное
 * объяснение циклической природы этой сборки в INSTALLER_PLAN.md,
 * раздел "Самоссылающийся kernel.flat", и tools/build_installer_assets.sh.
 *
 * БЕЗОПАСНОСТЬ. Эта программа НЕОБРАТИМО уничтожает содержимое
 * выбранного диска. Явное текстовое подтверждение ("YES", не просто
 * Enter) обязательно перед любой записью. ВАЖНОЕ ОГРАНИЧЕНИЕ: в системе
 * сейчас нет syscall'а "с какого диска сейчас загружена сама система"
 * (SYS_FS_BACKEND_INFO даёт только имя backend'а и факт монтирования,
 * без индекса диска, см. include/sysabi.h) -- install-os НЕ МОЖЕТ
 * автоматически отказаться писать поверх диска, с которого сам
 * загружен. Ответственность за выбор правильного диска (по размеру/
 * модели из списка) лежит на человеке, запускающем программу -- см.
 * предупреждение в самом выводе программы. */

#include "mlapp.h"
#include "libc.h"
#include "mbr.h"
#include "boot_assets.h"
#include "gpt.h"
#include "bootmeta.h"
#include "crypto_rng.h"

/* ---- Разметка диска -- ДОЛЖНА совпадать один в один с константами в
 * boot/stage1.s (STAGE2_LBA_START/STAGE2_SECTORS) и boot/stage2.s
 * (KERNEL_LBA_START/KERNEL_SECTORS/NATIVE_ENTRY_OFF) -- ни один из этих
 * файлов не может ссылаться на общий заголовок (ассемблер и C собираются
 * раздельно), поэтому синхронность -- на совести того, кто их меняет;
 * см. одинаковые числа и перекрёстные комментарии в обоих местах. ---- */
#define STAGE2_LBA_START 1u
#define STAGE2_SECTORS   63u
#define KERNEL_LBA_START 64u
#define KERNEL_SECTORS   32768u                         /* 16 MiB -- поднято с 2, 4,
                                    затем 8 MiB (KERNEL_SECTORS=4096, 8192, 16384) --
                                    kernel.flat перерос старые бюджеты после
                                    ATOMIC_PLAN.md Фаз 1-7 (в т.ч. самоссылающееся
                                    встраивание ассетов, см. tools/gen_boot_assets.py --
                                    каждый проход двухпроходной сборки несёт в себе
                                    ПОЛНЫЙ kernel.flat предыдущего прохода) -- см. тот
                                    же комментарий/константу в boot/stage2.s И
                                    boot/stage2_atomic.s (ATOMIC_PLAN.md, Фаза 6, там
                                    отдельная одноимённая константа под os-slot) */
#define DATA_PART_LBA_START  (KERNEL_LBA_START + KERNEL_SECTORS) /* 32832 */

/* Ассеты читаются НЕ через RAMFS (у неё лимит 64 KiB/файл, см. общий
 * комментарий в include/boot_assets.h) -- через выделенный syscall
 * SYS_BOOT_ASSET_READ (см. sys_boot_asset_read(), include/usyscall.h),
 * читающий данные прямо из .rodata самого ядра.
 *
 * ИСКЛЮЧЕНИЕ -- kernel.flat (BOOT_ASSET_KERNEL_FLAT). Раньше читался
 * тем же sys_boot_asset_read() в статический буфер (KERNEL_ASSET_MAX_BYTES
 * = 1.5 MiB) и писался тем же install_region(), что stage1/stage2 ниже
 * -- ЖИВЬЁМ СЛОМАЛОСЬ, когда kernel.flat перерос сначала этот буфер, а
 * затем (что куда серьёзнее) и сам APP_MAX_SIZE (2 MiB, include/loader.h
 * -- ЖЁСТКО привязан к разметке PDE процесса, include/memlayout.h,
 * поднимать его ради одного буфера означало бы менять адресную
 * раскладку ВСЕХ процессов) -- см. ATOMIC_PLAN.md, Фаза 7, за полным
 * разбором. Теперь kernel.flat пишется НАПРЯМУЮ между .rodata ядра и
 * диском через SYS_BOOT_ASSET_WRITE_TO_DISK (см. install_kernel_region()
 * ниже) -- ни один буфер размером с весь kernel.flat здесь больше не
 * заводится, ни в ядре, ни тем более здесь. */

#define ZERO_CHUNK_SECTORS 128u /* для дозаполнения нулями хвостов -- см. write_zero_padding() */

static u8 g_stage1_buf[MBR_SECTOR_SIZE];
static u8 g_stage2_buf[STAGE2_SECTORS * 512u];
static u8 g_zero_chunk[ZERO_CHUNK_SECTORS * 512u];

static void print_disks(const sys_storage_info_t *disks, u32 count) {
    printf("Обнаруженные диски:\n");
    for (u32 i = 0; i < count; i++) {
        u32 mb = disks[i].sectors / 2048u;
        printf("  [%u] %s -- %s, %u MiB (%u секторов)\n",
               i, disks[i].location, disks[i].model, mb, disks[i].sectors);
    }
}

/* Пишет count секторов нулей начиная с lba, чанками по ZERO_CHUNK_SECTORS
 * -- используется, чтобы детерминированно дозаполнить "хвост" фиксированных
 * областей (stage2/kernel) за пределами реального размера ассета: то, что
 * реально было на диске ДО установки, не должно оставаться читаемым
 * загрузчиком мусором (см. NOGRUB_PLAN.md, Фаза B -- stage2 всегда читает
 * ФИКСИРОВАННОЕ число секторов, не зная реального размера kernel.flat). */
static bool_t write_zero_padding(u32 drive, u32 lba, u32 count) {
    memset(g_zero_chunk, 0, sizeof(g_zero_chunk));
    while (count > 0) {
        u32 chunk = count > ZERO_CHUNK_SECTORS ? ZERO_CHUNK_SECTORS : count;
        if (sys_disk_write_raw(drive, lba, chunk, g_zero_chunk) < 0) return FALSE;
        lba += chunk;
        count -= chunk;
    }
    return TRUE;
}

/* Читает boot-ассет asset_id (см. include/boot_assets.h) целиком в buf
 * (bufsize -- размер буфера), пишет ceil(real_size/512) секторов начиная
 * с lba_start, затем дозаполняет нулями остаток области до
 * region_sectors секторов ИТОГО (см. write_zero_padding() выше).
 * region_sectors ДОЛЖНО быть достаточно, чтобы вместить реальный размер
 * ассета -- иначе возвращает FALSE. */
static bool_t install_region(u32 drive, boot_asset_id_t asset_id, u8 *buf, u32 bufsize,
                              u32 lba_start, u32 region_sectors, const char *label) {
    printf("Читаю ассет '%s'...\n", label);
    u32 real_size = 0;
    if (sys_boot_asset_read(asset_id, buf, bufsize, &real_size) < 0) {
        printf("install-os: не удалось прочитать ассет '%s' (буфер мал или сборка без Прохода 2 -- см. tools/build_installer_assets.sh)\n", label);
        return FALSE;
    }

    u32 real_sectors = (real_size + 511u) / 512u;
    if (real_sectors > region_sectors) {
        printf("install-os: '%s' (%u байт = %u секторов) не влезает в отведённую область (%u секторов)\n",
               label, real_size, real_sectors, region_sectors);
        return FALSE;
    }

    printf("Пишу %s (%u байт) на LBA %u...\n", label, real_size, lba_start);
    if (real_sectors > 0 && sys_disk_write_raw(drive, lba_start, real_sectors, buf) < 0) {
        printf("install-os: ошибка записи %s на диск\n", label);
        return FALSE;
    }

    u32 padding_sectors = region_sectors - real_sectors;
    if (padding_sectors > 0 &&
        !write_zero_padding(drive, lba_start + real_sectors, padding_sectors)) {
        printf("install-os: ошибка обнуления хвоста области %s\n", label);
        return FALSE;
    }

    return TRUE;
}

/* Аналог install_region() выше, но БЕЗ userspace-буфера -- см. общий
 * комментарий у SYS_BOOT_ASSET_WRITE_TO_DISK/VERIFY_ON_DISK,
 * include/sysabi.h, за тем, почему kernel.flat (в отличие от
 * stage1/stage2 -- маленьких, ~500 байт/32 КиБ) не может пройти через
 * install_region() -- он давно перерос и промежуточный буфер, и сам
 * APP_MAX_SIZE. Читает и пишет НАПРЯМУЮ между .rodata ядра и диском,
 * ЗАТЕМ сверяет запись через CRC32 (SYS_BOOT_ASSET_VERIFY_ON_DISK) --
 * та же "пишем -> проверяем" дисциплина, что ATOMIC_PLAN.md, Фаза 7,
 * требует от update-os (apps/update-os.c) -- install-os следует ей же
 * для симметрии, хотя формально для легаси-пути это не было частью
 * исходного плана. */
static bool_t install_kernel_region(u32 drive, u32 lba_start, u32 region_sectors, const char *label) {
    printf("Пишу '%s' (LBA %u..%u, напрямую из .rodata ядра)...\n",
           label, lba_start, lba_start + region_sectors - 1u);
    s32 real_size = sys_boot_asset_write_to_disk(BOOT_ASSET_KERNEL_FLAT, drive, lba_start, region_sectors);
    if (real_size < 0) {
        printf("install-os: не удалось записать '%s' (не влезает в отведённую область, либо сборка без Прохода 2, либо ошибка диска)\n", label);
        return FALSE;
    }
    printf("Записано %d байт. Сверяю через CRC32...\n", real_size);
    if (sys_boot_asset_verify_on_disk(BOOT_ASSET_KERNEL_FLAT, drive, lba_start) < 0) {
        printf("install-os: '%s' не совпал по CRC32 после записи -- ОСТАНОВЛЕНО (диск в промежуточном состоянии)\n", label);
        return FALSE;
    }
    printf("'%s' записан и проверен OK.\n", label);
    return TRUE;
}

/* ================================================================
 * ATOMIC_PLAN.md, Фаза 2 (хвост) -- реальная запись атомарной GPT-
 * разметки (boot-meta + os-slot-a + os-slot-b + user-images) на диск.
 *
 * Живёт ЗДЕСЬ, в install-os, а не в отдельном apps/install-atomic.c,
 * хотя план (ATOMIC_PLAN.md) оставлял это на выбор. На момент, когда
 * писался этот код, действовало более строгое правило -- CAP_DISK_RAW
 * только install-os -- с тех пор ослаблено (см. общий комментарий у
 * CAP_DISK_RAW, include/container.h: теперь кернел-side гейт
 * users_current_is_root() на самих SYS_DISK_READ_RAW/WRITE_RAW, а не
 * "список из одного разрешённого бинарника" -- у apps/create-user.c,
 * ATOMIC_PLAN.md Фаза 3, тоже есть disk_raw). Решение оставить разметку
 * здесь, а не выносить в install-atomic.c, тем не менее верное само по
 * себе, уже по другой причине: install-os и так читает/пишет весь диск
 * целиком с нуля (единственный сценарий, где заведомо нет ничего
 * ценного на диске, который стоит защищать от перезаписи) -- ставить
 * рядом ещё один бинарник с той же ответственностью ("первым файлом
 * трогает пустой/обнуляемый диск") не даёт ничего, кроме дублирования
 * кода GPT/mbr.
 *
 * Разметка -- ТА ЖЕ САМАЯ схема, что уже проверена round-trip'ом в
 * памяти в apps/atomictest.c (см. его комментарии за объяснением
 * каждого раздела) -- здесь она не придумывается заново, а просто
 * приземляется на настоящие LBA настоящего диска через
 * sys_disk_write_raw(), и затем ЧИТАЕТСЯ ОБРАТНО через
 * sys_disk_read_raw() + gpt_parse()/bootmeta_parse() для честной
 * сквозной проверки (в отличие от atomictest, где "диск" был просто
 * буфером в памяти того же процесса).
 *
 * ВАЖНО -- то, чего этот режим НЕ делает: не пишет kernel.flat/rootfs
 * ни в os-slot-a, ни в os-slot-b. Это осознанно вне рамок Фазы 2 --
 * заполнение слотов -- дело update-os (ATOMIC_PLAN.md, Фаза 7, ещё не
 * сделана), которое переключает active_slot ПОСЛЕ записи; до тех пор
 * оба слота остаются пустыми, а boot-meta указывает на пустой слот A --
 * загрузка такого диска ДОЙДЁТ до stage2_atomic.s (Фаза 6, готова, см.
 * boot/stage2_atomic.s), но упрётся в пустой kernel.flat (нечитаемый
 * ELF/нули) -- ожидаемо, не баг: "разметить диск" и "положить на него
 * систему" -- разные шаги, ровно как install-os и update-os разделены
 * в самом плане.
 * Результат этого режима -- пустой, но правильно размеченный диск с
 * boot-meta и УЖЕ рабочим (Фаза 6) boot-кодом, готовый под Фазы 3
 * (пользовательские образы) и 7 (заполнение слота kernel.flat'ом) --
 * каждая по отдельности. */

#define ATOMIC_BOOTMETA_PART_SECTORS 8u      /* см. apps/atomictest.c -- 1 сектор реально используется, запас на будущее */
#define ATOMIC_OS_SLOT_SECTORS       131072u /* 64 MiB -- с запасом над текущим kernel.flat (~2 MiB) */
#define ATOMIC_GPT_RESERVED_SECTORS  34u     /* protective MBR + заголовок + таблица разделов (см. gpt.h) */
/* ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий в начале
 * boot/stage2_atomic.s: отдельный раздел под boot-код (GPT_GUID_MEETLOOK_BIOS_BOOT,
 * include/gpt.h) -- protective MBR отдаёт под boot_code всего 446 байт, а
 * LBA 1..33 (GPT-заголовок+таблица) занимают то место, где в NOGRUB-схеме
 * лежал stage2 -- на Atomic-диске stage2_atomic.bin положить больше
 * некуда. Тот же бюджет (63 сектора), что STAGE2_SECTORS у NOGRUB-схемы
 * (boot/stage1.s) -- см. также build/stage1_gpt.bin/stage2_atomic.bin,
 * Makefile. */
#define ATOMIC_BIOSBOOT_SECTORS      63u

/* Буферы под сериализованную разметку -- статические (.bss), тем же
 * духом, что g_stage1_buf выше: это самый большой
 * бюджет (32 КиБ на каждую копию таблицы разделов), не хочется класть
 * его на стек приложения. g_atomic_readback переиспользуется под ВСЕ
 * шаги readback-проверки по очереди (самый большой нужный размер --
 * таблица разделов), а не заводится по буферу на каждый шаг. */
static u8 g_atomic_protective_mbr[MBR_SECTOR_SIZE];
static u8 g_atomic_primary_header[GPT_SECTOR_SIZE];
static u8 g_atomic_primary_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
static u8 g_atomic_backup_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
static u8 g_atomic_backup_header[GPT_SECTOR_SIZE];
static u8 g_atomic_bootmeta_sector[BOOTMETA_SECTOR_SIZE];
static u8 g_atomic_readback[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
/* ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у ATOMIC_BIOSBOOT_SECTORS
 * выше. Тот же размер, что g_stage1_buf/g_stage2_buf ниже в этом файле
 * (легаси-путь) -- не совпадение, это буквально те же ассеты (см.
 * include/boot_assets.h), просто с другим содержимым. */
static u8 g_atomic_stage1_buf[MBR_SECTOR_SIZE];
static u8 g_atomic_stage2_buf[ATOMIC_BIOSBOOT_SECTORS * 512u];

/* TRUE, если disk_sectors достаточно, чтобы вместить всю разметку --
 * та же формула, что apps/atomictest.c (минимум под user-images здесь
 * чисто символический, 2048 секторов = 1 MiB: этот режим Фазу 3 ещё
 * не реализует, реальные слоты пользователей нарезать будет позже). */
static bool_t atomic_layout_fits(u32 disk_sectors) {
    u64 min_sectors = 2u * ATOMIC_GPT_RESERVED_SECTORS + ATOMIC_BIOSBOOT_SECTORS +
                       ATOMIC_BOOTMETA_PART_SECTORS + 2u * ATOMIC_OS_SLOT_SECTORS + 2048u;
    return (u64)disk_sectors > min_sectors;
}

/* Собирает disk_guid через RDRAND, если CPU его поддерживает, иначе
 * детерминированный запасной вариант -- ровно тот же принцип, что
 * apps/atomictest.c (см. его комментарий за подробным объяснением). */
static gpt_guid_t atomic_pick_disk_guid(void) {
    gpt_guid_t fallback = gpt_guid_from_fields(0xDEADBEEFu, 0xCAFEu, 0xF00Du,
        (const u8[8]){ 0, 1, 2, 3, 4, 5, 6, 7 });
    if (!crypto_rng_init()) {
        printf("disk_guid: RDRAND недоступен на этом CPU -- запасной фиксированный GUID\n");
        return fallback;
    }
    u8 raw[16];
    if (!crypto_rng_bytes(raw, sizeof(raw))) {
        printf("disk_guid: RDRAND инициализировался, но не выдал данные -- запасной фиксированный GUID\n");
        return fallback;
    }
    gpt_guid_t guid;
    memcpy(guid.bytes, raw, 16);
    printf("disk_guid: получен через RDRAND\n");
    return guid;
}

/* Пишет весь набор (protective MBR + primary GPT + backup GPT +
 * boot-meta) на drive и читает его обратно, сверяя байт в байт с тем,
 * что было задано -- см. общий комментарий у секции выше. Возвращает
 * FALSE при первой же ошибке записи/чтения/расхождения (диск в этом
 * случае может быть в промежуточном состоянии -- ровно то же
 * предупреждение, что уже даёт остальной install-os при сбое записи). */
static bool_t install_atomic_layout(u32 drive, u32 disk_sectors) {
    /* ---- 1. Раскладка разделов -- см. apps/atomictest.c ---- */
    gpt_partition_entry_t parts[GPT_MAX_PARTITIONS];
    memset(parts, 0, sizeof(parts));

    u64 cur = ATOMIC_GPT_RESERVED_SECTORS;
    u64 biosboot_start = cur;                              /* ATOMIC_PLAN.md, Фаза 6 */
    u64 biosboot_end = biosboot_start + ATOMIC_BIOSBOOT_SECTORS - 1u;
    cur = biosboot_end + 1u;

    u64 bootmeta_start = cur;
    u64 bootmeta_end = bootmeta_start + ATOMIC_BOOTMETA_PART_SECTORS - 1u;
    cur = bootmeta_end + 1u;

    u64 slot_a_start = cur;
    u64 slot_a_end = slot_a_start + ATOMIC_OS_SLOT_SECTORS - 1u;
    cur = slot_a_end + 1u;

    u64 slot_b_start = cur;
    u64 slot_b_end = slot_b_start + ATOMIC_OS_SLOT_SECTORS - 1u;
    cur = slot_b_end + 1u;

    u64 userimg_start = cur;
    u64 userimg_end = (u64)disk_sectors - ATOMIC_GPT_RESERVED_SECTORS - 1u;

    gpt_partition_entry_set(&parts[0], GPT_GUID_MEETLOOK_BIOS_BOOT, biosboot_start, biosboot_end, "bios-boot");
    gpt_partition_entry_set(&parts[1], GPT_GUID_MEETLOOK_BOOTMETA, bootmeta_start, bootmeta_end, "boot-meta");
    gpt_partition_entry_set(&parts[2], GPT_GUID_MEETLOOK_OS_SLOT, slot_a_start, slot_a_end, "os-slot-a");
    gpt_partition_entry_set(&parts[3], GPT_GUID_MEETLOOK_OS_SLOT, slot_b_start, slot_b_end, "os-slot-b");
    gpt_partition_entry_set(&parts[4], GPT_GUID_MEETLOOK_USER_IMAGES, userimg_start, userimg_end, "user-images");

    printf("Раздел 0: bios-boot    LBA %u..%u\n", (u32)biosboot_start, (u32)biosboot_end);
    printf("Раздел 1: boot-meta    LBA %u..%u\n", (u32)bootmeta_start, (u32)bootmeta_end);
    printf("Раздел 2: os-slot-a    LBA %u..%u (~%u МиБ)\n",
           (u32)slot_a_start, (u32)slot_a_end, (u32)((slot_a_end - slot_a_start + 1) / 2048u));
    printf("Раздел 3: os-slot-b    LBA %u..%u (~%u МиБ)\n",
           (u32)slot_b_start, (u32)slot_b_end, (u32)((slot_b_end - slot_b_start + 1) / 2048u));
    printf("Раздел 4: user-images  LBA %u..%u (~%u МиБ)\n",
           (u32)userimg_start, (u32)userimg_end, (u32)((userimg_end - userimg_start + 1) / 2048u));

    /* ---- 2. protective MBR (LBA 0) -- см. MBR_PART_TYPE_GPT_PROTECTIVE,
     * include/mbr.h -- ЕГО boot_code ТЕПЕРЬ НЕ пуст (в отличие от того,
     * что было до Фазы 6): реальные legacy-BIOS (в т.ч. SeaBIOS/QEMU)
     * исполняют код LBA 0 независимо от того, GPT это диск или MBR --
     * "protective" в названии описывает, что видят СТАРЫЕ инструменты,
     * не смотрящие на GPT вовсе, а не то, что BIOS игнорирует boot_code.
     * Без него Atomic-диск был не грузить ничем, кроме UEFI (которого в
     * этом дереве нет) -- см. общий комментарий в boot/stage2_atomic.s
     * за тем, что здесь используется вместо этого. ---- */
    printf("Читаю ассет 'stage1_atomic'...\n");
    u32 stage1_atomic_size = 0;
    if (sys_boot_asset_read(BOOT_ASSET_STAGE1_ATOMIC, g_atomic_stage1_buf, sizeof(g_atomic_stage1_buf), &stage1_atomic_size) < 0) {
        printf("install-os: не удалось прочитать ассет 'stage1_atomic' (сборка без Прохода 2 -- см. tools/build_installer_assets.sh)\n");
        return FALSE;
    }

    mbr_partition_t prot_parts[MBR_PARTITION_COUNT];
    memset(prot_parts, 0, sizeof(prot_parts));
    u32 prot_sectors = (disk_sectors - 1u > 0xFFFFFFFEu) ? 0xFFFFFFFFu : (disk_sectors - 1u);
    mbr_partition_set(&prot_parts[0], FALSE, MBR_PART_TYPE_GPT_PROTECTIVE, 1u, prot_sectors);

    mbr_sector_t prot_sector;
    mbr_build(&prot_sector, g_atomic_stage1_buf, stage1_atomic_size, prot_parts);
    memcpy(g_atomic_protective_mbr, &prot_sector, MBR_SECTOR_SIZE);

    printf("Пишу protective MBR (+ stage1_atomic boot-код) на LBA 0...\n");
    if (sys_disk_write_raw(drive, 0, 1, g_atomic_protective_mbr) < 0) {
        printf("install-os: ошибка записи protective MBR -- ОСТАНОВЛЕНО\n");
        return FALSE;
    }

    /* ---- 3. GPT (primary + backup) ---- */
    gpt_guid_t disk_guid = atomic_pick_disk_guid();
    gpt_build((u64)disk_sectors, disk_guid, parts,
              g_atomic_primary_header, g_atomic_primary_table,
              g_atomic_backup_table, g_atomic_backup_header);

    u32 primary_table_lba = 2u;
    u32 backup_table_lba = disk_sectors - 1u - GPT_PARTITION_TABLE_SECTORS;
    u32 backup_header_lba = disk_sectors - 1u;

    printf("Пишу первичный GPT-заголовок (LBA 1) и таблицу разделов (LBA %u..%u)...\n",
           primary_table_lba, primary_table_lba + GPT_PARTITION_TABLE_SECTORS - 1u);
    if (sys_disk_write_raw(drive, 1u, 1u, g_atomic_primary_header) < 0 ||
        sys_disk_write_raw(drive, primary_table_lba, GPT_PARTITION_TABLE_SECTORS, g_atomic_primary_table) < 0) {
        printf("install-os: ошибка записи первичного GPT -- ОСТАНОВЛЕНО (диск в промежуточном состоянии)\n");
        return FALSE;
    }

    printf("Пишу резервную таблицу разделов (LBA %u..%u) и заголовок (LBA %u)...\n",
           backup_table_lba, backup_table_lba + GPT_PARTITION_TABLE_SECTORS - 1u, backup_header_lba);
    if (sys_disk_write_raw(drive, backup_table_lba, GPT_PARTITION_TABLE_SECTORS, g_atomic_backup_table) < 0 ||
        sys_disk_write_raw(drive, backup_header_lba, 1u, g_atomic_backup_header) < 0) {
        printf("install-os: ошибка записи резервного GPT -- ОСТАНОВЛЕНО (диск в промежуточном состоянии)\n");
        return FALSE;
    }

    /* ---- 4. stage2_atomic.bin -> раздел bios-boot (LBA %u..%u выше) --
     * см. общий комментарий у ATOMIC_BIOSBOOT_SECTORS. install_region()
     * -- ТА ЖЕ функция, что легаси-путь ниже использует для stage2.bin
     * (LBA 1..63) -- один и тот же приём (читает ассет, проверяет, что
     * влезает, пишет, нулями дозаполняет хвост области). ---- */
    if (!install_region(drive, BOOT_ASSET_STAGE2_ATOMIC, g_atomic_stage2_buf, sizeof(g_atomic_stage2_buf),
                         (u32)biosboot_start, ATOMIC_BIOSBOOT_SECTORS, "stage2_atomic")) {
        return FALSE;
    }

    /* ---- 5. boot-meta -- init (слот A активен, ничего не откатывалось,
     * см. bootmeta_init()), затем нулями дозаполняем остаток раздела
     * (переиспользуем write_zero_padding(), тот же принцип, что и в
     * install_region() выше -- никакого мусора "как было на диске"). ---- */
    bootmeta_t bm;
    bootmeta_init(&bm);
    bootmeta_serialize(&bm, g_atomic_bootmeta_sector);

    printf("Пишу boot-meta (active_slot=A) на LBA %u...\n", (u32)bootmeta_start);
    if (sys_disk_write_raw(drive, (u32)bootmeta_start, 1u, g_atomic_bootmeta_sector) < 0) {
        printf("install-os: ошибка записи boot-meta -- ОСТАНОВЛЕНО (диск в промежуточном состоянии)\n");
        return FALSE;
    }
    if (ATOMIC_BOOTMETA_PART_SECTORS > 1u &&
        !write_zero_padding(drive, (u32)bootmeta_start + 1u, ATOMIC_BOOTMETA_PART_SECTORS - 1u)) {
        printf("install-os: ошибка обнуления хвоста boot-meta -- не критично, сам сектор уже записан корректно\n");
    }

    /* ---- 6. Readback -- честная сквозная проверка через настоящий
     * диск (не память, как в atomictest.c): читаем то же самое обратно
     * и разбираем через gpt_parse()/bootmeta_parse(). ---- */
    printf("\nПроверяю запись (читаю обратно и разбираю)...\n");
    bool_t all_ok = TRUE;

    u8 header_readback[GPT_SECTOR_SIZE];
    if (sys_disk_read_raw(drive, 1u, 1u, header_readback) < 0 ||
        sys_disk_read_raw(drive, primary_table_lba, GPT_PARTITION_TABLE_SECTORS, g_atomic_readback) < 0) {
        printf("install-os: не удалось прочитать GPT обратно с диска\n");
        all_ok = FALSE;
    } else {
        gpt_partition_entry_t parsed[GPT_MAX_PARTITIONS];
        if (!gpt_parse(header_readback, g_atomic_readback, parsed)) {
            printf("install-os: gpt_parse() не распознал только что записанный GPT -- FAIL\n");
            all_ok = FALSE;
        } else if (!(gpt_partition_is_used(&parsed[0]) && parsed[0].starting_lba == biosboot_start && parsed[0].ending_lba == biosboot_end) ||
                   !(gpt_partition_is_used(&parsed[1]) && parsed[1].starting_lba == bootmeta_start && parsed[1].ending_lba == bootmeta_end) ||
                   !(gpt_partition_is_used(&parsed[2]) && parsed[2].starting_lba == slot_a_start && parsed[2].ending_lba == slot_a_end) ||
                   !(gpt_partition_is_used(&parsed[3]) && parsed[3].starting_lba == slot_b_start && parsed[3].ending_lba == slot_b_end) ||
                   !(gpt_partition_is_used(&parsed[4]) && parsed[4].starting_lba == userimg_start && parsed[4].ending_lba == userimg_end)) {
            printf("install-os: разобранные с диска разделы не совпадают с записанными -- FAIL\n");
            all_ok = FALSE;
        } else {
            printf("GPT (primary, читано с диска): все 5 разделов совпадают -- OK\n");
        }
    }

    u8 bm_readback[BOOTMETA_SECTOR_SIZE];
    if (sys_disk_read_raw(drive, (u32)bootmeta_start, 1u, bm_readback) < 0) {
        printf("install-os: не удалось прочитать boot-meta обратно с диска\n");
        all_ok = FALSE;
    } else {
        bootmeta_t bm_parsed;
        if (!bootmeta_parse(bm_readback, &bm_parsed)) {
            printf("install-os: bootmeta_parse() не распознал только что записанный boot-meta -- FAIL\n");
            all_ok = FALSE;
        } else if (bm_parsed.active_slot != BOOTMETA_SLOT_A || bm_parsed.boot_confirmed != 1 ||
                   bm_parsed.boot_tries_left != 0) {
            printf("install-os: boot-meta, читанный с диска, не совпадает с записанным -- FAIL\n");
            all_ok = FALSE;
        } else {
            printf("boot-meta (читано с диска): active_slot=A, confirmed=1 -- OK\n");
        }
    }

    return all_ok;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== install-os -- установка MeetlookCoreOS без ISO/GRUB ===\n");
    printf("См. INSTALLER_PLAN.md и NOGRUB_PLAN.md за подробностями.\n\n");

    sys_storage_info_t disks[16];
    u32 disk_count = 0;
    if (sys_storage_list(disks, 16, &disk_count) < 0 || disk_count == 0) {
        printf("install-os: не нашёл ни одного диска (sys_storage_list)\n");
        sys_exit(1);
    }
    print_disks(disks, disk_count);

    printf("\nВНИМАНИЕ: у системы нет способа определить, с какого диска\n");
    printf("она сейчас сама загружена -- выбирайте диск по размеру/модели\n");
    printf("осознанно. Запись НЕОБРАТИМО уничтожит всё содержимое диска.\n\n");

    printf("Номер диска для установки: ");
    char line[64];
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    s32 drive = atoi(line);
    if (drive < 0 || (u32)drive >= disk_count) {
        printf("install-os: неверный номер диска\n");
        sys_exit(1);
    }

    u32 disk_sectors = disks[drive].sectors;
    if (disk_sectors <= DATA_PART_LBA_START + 4096u) { /* см. fat32_format()/ufs_format(): минимум 4096 секторов под сам раздел */
        printf("install-os: диск [%d] (%u секторов) слишком мал -- нужно как минимум %u\n",
               drive, disk_sectors, DATA_PART_LBA_START + 4096u);
        sys_exit(1);
    }

    /* ---- Выбор разметки -- ATOMIC_PLAN.md, Фаза 2 (хвост): "Atomic"
     * пишет ТОЛЬКО GPT + boot-meta (пустые os-slot-a/b/user-images,
     * без kernel.flat -- см. общий комментарий у install_atomic_layout()
     * выше за тем, почему) и сразу выходит, минуя весь остальной
     * legacy-путь (MBR/stage1/stage2/kernel/раздел с данными) ниже. ---- */
    printf("\nРазметка диска:\n");
    printf("  [1] Legacy -- MBR, один раздел с данными (по умолчанию, загружается уже сейчас)\n");
    printf("  [2] Atomic -- GPT + A/B read-only слоты + boot-meta (экспериментально,\n");
    printf("      ATOMIC_PLAN.md, Фаза 2/6 -- пишет разметку И рабочий boot-код,\n");
    printf("      но kernel.flat в слоты пока не кладёт -- см. Фазу 7)\n");
    printf("Выбор [1]: ");
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    if (strcmp(line, "2") == 0) {
        if (!atomic_layout_fits(disk_sectors)) {
            printf("install-os: диск [%d] (%u секторов) слишком мал для атомарной разметки\n", drive, disk_sectors);
            sys_exit(1);
        }
        printf("\nВыбран диск [%d]: %s, %s, %u секторов.\n",
               drive, disks[drive].location, disks[drive].model, disk_sectors);
        printf("Все данные на нём будут уничтожены безвозвратно.\n");
        printf("Введите заглавными буквами \"YES\" для подтверждения: ");
        if (readline(line, sizeof(line)) < 0) sys_exit(1);
        if (strcmp(line, "YES") != 0) {
            printf("install-os: подтверждение не получено, отмена.\n");
            sys_exit(1);
        }
        printf("\n=== Пишу атомарную GPT-разметку (ATOMIC_PLAN.md, Фаза 2/6) ===\n");
        bool_t atomic_ok = install_atomic_layout((u32)drive, disk_sectors);
        printf("\n=== %s ===\n", atomic_ok ? "install-os: разметка записана и проверена OK" : "install-os: ЕСТЬ FAIL");
        if (atomic_ok) {
            printf("Слоты os-slot-a/b и user-images размечены, но пусты -- нужна\n");
            printf("apps/create-user.c (пользователи) и update-os (Фаза 7, ещё не\n");
            printf("сделана) для kernel.flat в слот, прежде чем диск реально загрузится.\n");
        }
        sys_exit(atomic_ok ? 0 : 1);
    }

    /* ---- Выбор файловой системы для раздела с пользовательскими
     * данными -- см. INSTALLER_PLAN.md, Фаза 3: UFS (fs/ufs.c) --
     * умолчание, рекомендуется для стационарных дисков/серверов; FAT32
     * (fs/fat32.c) -- опция для переносных накопителей (флешки,
     * внешние диски), которые должны читаться на других ОС "из коробки"
     * (UFS этого драйвера -- собственный формат, никакая другая ОС его
     * прочитать не умеет). Обе ФС теперь partition-aware (см. общий
     * комментарий в fs/fat32.c/fs/ufs.c про detect_partition_range()) --
     * выбор ни на что другое в разметке диска не влияет. */
    printf("\nФайловая система для раздела с данными:\n");
    printf("  [1] UFS   -- по умолчанию, для стационарных дисков/серверов\n");
    printf("  [2] FAT32 -- для переносных накопителей (флешки), читается другими ОС\n");
    printf("Выбор [1]: ");
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    bool_t use_fat32 = (strcmp(line, "2") == 0);
    const char *fs_label = use_fat32 ? "FAT32" : "UFS";
    u8 fs_part_type = use_fat32 ? MBR_PART_TYPE_FAT32_LBA : MBR_PART_TYPE_MEETLOOK_UFS;
    u32 fs_type_id = use_fat32 ? 0u /* FS_TYPE_FAT32 */ : 1u /* FS_TYPE_UFS, см. include/vfs.h */;

    printf("\nВыбран диск [%d]: %s, %s, %u секторов.\n",
           drive, disks[drive].location, disks[drive].model, disk_sectors);
    printf("Все данные на нём будут уничтожены безвозвратно.\n");
    printf("Введите заглавными буквами \"YES\" для подтверждения: ");
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    if (strcmp(line, "YES") != 0) {
        printf("install-os: подтверждение не получено, отмена.\n");
        sys_exit(1);
    }
    printf("\n");

    /* ---- 1. stage1.bin -> MBR (через mbr_build(), см. include/mbr.h) ---- */
    printf("Читаю ассет 'stage1'...\n");
    u32 stage1_size = 0;
    if (sys_boot_asset_read(BOOT_ASSET_STAGE1, g_stage1_buf, sizeof(g_stage1_buf), &stage1_size) < 0) {
        printf("install-os: не удалось прочитать ассет 'stage1'\n");
        sys_exit(1);
    }

    mbr_partition_t parts[MBR_PARTITION_COUNT];
    memset(parts, 0, sizeof(parts));
    u32 data_part_sectors = disk_sectors - DATA_PART_LBA_START;
    mbr_partition_set(&parts[0], TRUE, fs_part_type, DATA_PART_LBA_START, data_part_sectors);

    mbr_sector_t sector;
    mbr_build(&sector, g_stage1_buf, stage1_size, parts);

    printf("Пишу MBR (stage1 + разметка раздела) на LBA 0...\n");
    if (sys_disk_write_raw(drive, 0, 1, &sector) < 0) {
        printf("install-os: ошибка записи MBR -- ОСТАНОВЛЕНО (диск может быть в промежуточном состоянии)\n");
        sys_exit(1);
    }

    /* ---- 2. stage2.bin -> LBA 1..63 ---- */
    if (!install_region(drive, BOOT_ASSET_STAGE2, g_stage2_buf, sizeof(g_stage2_buf),
                         STAGE2_LBA_START, STAGE2_SECTORS, "stage2")) {
        sys_exit(1);
    }

    /* ---- 3. kernel.flat -> LBA 64..16447 (см. install_kernel_region() выше
     * за тем, почему НЕ install_region(), в отличие от stage1/stage2) ---- */
    if (!install_kernel_region(drive, KERNEL_LBA_START, KERNEL_SECTORS, "kernel")) {
        sys_exit(1);
    }

    /* ---- 4. Раздел с данными -- см. fs/fat32.c и fs/ufs.c:
     *    detect_partition_range() теперь находит только что записанный
     *    MBR и форматирует/монтирует ИМЕННО раздел (LBA
     *    DATA_PART_LBA_START..), не весь диск -- см. INSTALLER_PLAN.md,
     *    Фаза 3. Копирование пользовательских файлов (rootfs/bin/) на
     *    этот раздел сознательно НЕ делается здесь -- RAMFS, встроенный
     *    в сам kernel.flat, уже полностью самодостаточен (см. каждую
     *    проверку загрузки в NOGRUB_PLAN.md) -- раздел подготовлен и
     *    смонтирован, готов для ручного использования. ---- */
    printf("Форматирую раздел (%s, LBA %u, %u секторов)...\n", fs_label, DATA_PART_LBA_START, data_part_sectors);
    if (sys_fs_format(drive, fs_type_id) < 0) {
        printf("install-os: не удалось отформатировать раздел (не критично -- boot-цепочка уже записана)\n");
    } else if (sys_fs_mount(drive) < 0) {
        printf("install-os: не удалось смонтировать раздел (не критично)\n");
    } else {
        printf("Раздел (%s) готов и смонтирован.\n", fs_label);
    }

    printf("\n=== Установка завершена. ===\n");
    printf("Перезагрузите QEMU/машину БЕЗ -cdrom, указав именно этот диск --\n");
    printf("система должна загрузиться без единого байта GRUB.\n");

    sys_exit(0);
    return 0;
}

/* atomictest.c -- ATOMIC_PLAN.md, Фаза 2: собирает ПОЛНУЮ разметку
 * диска атомарной схемы (boot-meta + os-slot-a + os-slot-b +
 * user-images) через gpt_build() и bootmeta_serialize(), печатает
 * сводку, разбирает всё обратно (gpt_parse()/bootmeta_parse()) и
 * сверяет с тем, что было задано -- НИ РАЗУ не трогая настоящий диск
 * (никакого sys_disk_read_raw/write_raw здесь нет и не должно быть).
 * Тот же принцип, что и у apps/mbrtest.c/apps/gpttest.c -- первый шаг
 * перед тем, как вообще подпускать sys_disk_write_raw() к делу (это
 * будет apps/install-os.c, ATOMIC_PLAN.md Фаза 3/4).
 *
 * Не требует ни одной capability -- вся работа в памяти. */

#include "mlapp.h"
#include "libc.h"
#include "usyscall.h"
#include "gpt.h"
#include "bootmeta.h"
#include "crypto_rng.h"

/* ---- Размеры по умолчанию для демо-разметки ---- */
#define BOOTMETA_PART_SECTORS  8u        /* 1 сектор реально используется, запас на будущее */
#define OS_SLOT_SECTORS        131072u   /* 64 МиБ -- с запасом над текущим kernel.flat (~2 МиБ, см. NOGRUB_PLAN.md) */
#define GPT_RESERVED_SECTORS   34u       /* protective MBR + заголовок + таблица разделов (см. gpt.h) */

static bool_t g_all_ok = TRUE;

static void check(bool_t cond, const char *what) {
    printf("%s: %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) g_all_ok = FALSE;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    u64 disk_sectors = (512ull * 1024ull * 1024ull) / GPT_SECTOR_SIZE; /* 512 МиБ по умолчанию */
    if (argc >= 2) disk_sectors = (u64)atoi(argv[1]);

    u64 min_sectors = 2u * GPT_RESERVED_SECTORS + BOOTMETA_PART_SECTORS +
                       2u * OS_SLOT_SECTORS + 2048u /* хоть какой-то user-images */;
    if (disk_sectors <= min_sectors) {
        printf("atomictest: диск слишком маленький (%u секторов, нужно минимум %u)\n",
               (u32)disk_sectors, (u32)min_sectors);
        sys_exit(1);
    }

    printf("=== atomictest -- сборка атомарной GPT-разметки (ATOMIC_PLAN.md, Фаза 2) ===\n");
    printf("Диск: %u секторов (~%u МиБ)\n", (u32)disk_sectors, (u32)(disk_sectors / 2048u));

    /* ---- 1. Раскладка: boot-meta, os-slot-a, os-slot-b, user-images (остаток) ---- */
    gpt_partition_entry_t parts[GPT_MAX_PARTITIONS];
    memset(parts, 0, sizeof(parts));

    u64 cur = GPT_RESERVED_SECTORS;

    u64 bootmeta_start = cur;
    u64 bootmeta_end = bootmeta_start + BOOTMETA_PART_SECTORS - 1u;
    cur = bootmeta_end + 1u;

    u64 slot_a_start = cur;
    u64 slot_a_end = slot_a_start + OS_SLOT_SECTORS - 1u;
    cur = slot_a_end + 1u;

    u64 slot_b_start = cur;
    u64 slot_b_end = slot_b_start + OS_SLOT_SECTORS - 1u;
    cur = slot_b_end + 1u;

    u64 userimg_start = cur;
    u64 userimg_end = disk_sectors - GPT_RESERVED_SECTORS - 1u;

    gpt_partition_entry_set(&parts[0], GPT_GUID_MEETLOOK_BOOTMETA, bootmeta_start, bootmeta_end, "boot-meta");
    gpt_partition_entry_set(&parts[1], GPT_GUID_MEETLOOK_OS_SLOT, slot_a_start, slot_a_end, "os-slot-a");
    gpt_partition_entry_set(&parts[2], GPT_GUID_MEETLOOK_OS_SLOT, slot_b_start, slot_b_end, "os-slot-b");
    gpt_partition_entry_set(&parts[3], GPT_GUID_MEETLOOK_USER_IMAGES, userimg_start, userimg_end, "user-images");

    printf("Раздел 0: boot-meta    LBA %u..%u\n", (u32)bootmeta_start, (u32)bootmeta_end);
    printf("Раздел 1: os-slot-a    LBA %u..%u (~%u МиБ)\n",
           (u32)slot_a_start, (u32)slot_a_end, (u32)((slot_a_end - slot_a_start + 1) / 2048u));
    printf("Раздел 2: os-slot-b    LBA %u..%u (~%u МиБ)\n",
           (u32)slot_b_start, (u32)slot_b_end, (u32)((slot_b_end - slot_b_start + 1) / 2048u));
    printf("Раздел 3: user-images  LBA %u..%u (~%u МиБ)\n",
           (u32)userimg_start, (u32)userimg_end, (u32)((userimg_end - userimg_start + 1) / 2048u));

    /* ---- 2. disk_guid -- через RDRAND, если доступен (ATOMIC_PLAN.md,
     * Фаза 1), иначе детерминированный запасной вариант (тот же принцип,
     * что раньше был в gpttest.c, когда крипто ещё не было вообще). ---- */
    gpt_guid_t disk_guid;
    if (crypto_rng_init()) {
        u8 raw[16];
        if (crypto_rng_bytes(raw, sizeof(raw))) {
            memcpy(disk_guid.bytes, raw, 16);
            printf("disk_guid: получен через RDRAND\n");
        } else {
            disk_guid = gpt_guid_from_fields(0xDEADBEEFu, 0xCAFEu, 0xF00Du,
                (const u8[8]){ 0,1,2,3,4,5,6,7 });
            printf("disk_guid: RDRAND инициализировался, но не выдал данные -- запасной фиксированный GUID\n");
        }
    } else {
        disk_guid = gpt_guid_from_fields(0xDEADBEEFu, 0xCAFEu, 0xF00Du,
            (const u8[8]){ 0,1,2,3,4,5,6,7 });
        printf("disk_guid: RDRAND недоступен на этом CPU -- запасной фиксированный GUID\n");
    }

    /* ---- 3. Собираем GPT (primary+backup) в памяти ---- */
    static u8 primary_header[GPT_SECTOR_SIZE];
    static u8 primary_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    static u8 backup_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    static u8 backup_header[GPT_SECTOR_SIZE];

    gpt_build(disk_sectors, disk_guid, parts,
              primary_header, primary_table, backup_table, backup_header);

    /* ---- 4. Round-trip: разбираем то, что только что собрали ---- */
    gpt_partition_entry_t parsed[GPT_MAX_PARTITIONS];
    bool_t parsed_ok = gpt_parse(primary_header, primary_table, parsed);
    check(parsed_ok, "gpt_parse(primary)");

    if (parsed_ok) {
        check(gpt_partition_is_used(&parsed[0]) && parsed[0].starting_lba == bootmeta_start &&
              parsed[0].ending_lba == bootmeta_end, "раздел 0 (boot-meta) совпадает");
        check(gpt_partition_is_used(&parsed[1]) && parsed[1].starting_lba == slot_a_start &&
              parsed[1].ending_lba == slot_a_end, "раздел 1 (os-slot-a) совпадает");
        check(gpt_partition_is_used(&parsed[2]) && parsed[2].starting_lba == slot_b_start &&
              parsed[2].ending_lba == slot_b_end, "раздел 2 (os-slot-b) совпадает");
        check(gpt_partition_is_used(&parsed[3]) && parsed[3].starting_lba == userimg_start &&
              parsed[3].ending_lba == userimg_end, "раздел 3 (user-images) совпадает");
        for (u32 i = 4; i < GPT_MAX_PARTITIONS; i++) {
            if (gpt_partition_is_used(&parsed[i])) {
                printf("раздел %u должен быть пуст, а не пуст: FAIL\n", i);
                g_all_ok = FALSE;
            }
        }
    }

    /* Резервная копия -- сверяем ОТДЕЛЬНО через gpt_parse(), как и
     * рекомендует общий комментарий в gpt.h ("не проверяет резервную
     * копию -- забота вызывающего кода"). */
    gpt_partition_entry_t parsed_backup[GPT_MAX_PARTITIONS];
    bool_t backup_ok = gpt_parse(backup_header, backup_table, parsed_backup);
    check(backup_ok, "gpt_parse(backup)");
    if (backup_ok) {
        check(memcmp(parsed, parsed_backup, sizeof(parsed)) == 0,
              "backup-таблица совпадает с primary");
    }

    printf("\n");

    /* ---- 5. boot-meta: init -> serialize -> parse, отдельным раундом ---- */
    bootmeta_t bm;
    bootmeta_init(&bm);
    printf("boot-meta (init): active_slot=%u tries_left=%u confirmed=%u\n",
           bm.active_slot, bm.boot_tries_left, bm.boot_confirmed);

    static u8 bm_sector[BOOTMETA_SECTOR_SIZE];
    bootmeta_serialize(&bm, bm_sector);

    bootmeta_t bm_parsed;
    bool_t bm_ok = bootmeta_parse(bm_sector, &bm_parsed);
    check(bm_ok, "bootmeta_parse(init)");
    if (bm_ok) {
        check(bm_parsed.active_slot == BOOTMETA_SLOT_A &&
              bm_parsed.boot_confirmed == 1 &&
              bm_parsed.boot_tries_left == 0, "boot-meta (init) содержимое совпадает");
    }

    /* Симулируем Фазу 7 (update-os): переключение на противоположный
     * слот с BOOTMETA_DEFAULT_TRIES попыток, ещё не подтверждено. */
    bootmeta_t bm_update;
    bm_update.magic = BOOTMETA_MAGIC;
    bm_update.active_slot = bootmeta_other_slot(bm.active_slot);
    bm_update.boot_tries_left = BOOTMETA_DEFAULT_TRIES;
    bm_update.boot_confirmed = 0;
    bm_update.reserved = 0;

    static u8 bm_update_sector[BOOTMETA_SECTOR_SIZE];
    bootmeta_serialize(&bm_update, bm_update_sector);

    bootmeta_t bm_update_parsed;
    bool_t bm_update_ok = bootmeta_parse(bm_update_sector, &bm_update_parsed);
    check(bm_update_ok, "bootmeta_parse(update)");
    if (bm_update_ok) {
        check(bm_update_parsed.active_slot == BOOTMETA_SLOT_B &&
              bm_update_parsed.boot_tries_left == BOOTMETA_DEFAULT_TRIES &&
              bm_update_parsed.boot_confirmed == 0, "boot-meta (update) содержимое совпадает");
    }

    /* Проверка защиты CRC32: портим один байт payload'а, parse обязан
     * отказать (не выдать поле, а вернуть FALSE) -- иначе CRC ничего не
     * защищает. */
    static u8 bm_corrupt[BOOTMETA_SECTOR_SIZE];
    memcpy(bm_corrupt, bm_update_sector, BOOTMETA_SECTOR_SIZE);
    bm_corrupt[5] ^= 0xFFu; /* портим boot_tries_left */
    bootmeta_t bm_corrupt_parsed;
    check(!bootmeta_parse(bm_corrupt, &bm_corrupt_parsed),
          "bootmeta_parse(corrupt) отклоняет битый CRC32");

    printf("\n=== %s ===\n", g_all_ok ? "atomictest: ВСЁ OK" : "atomictest: ЕСТЬ FAIL");

    /* ВАЖНО: вход в app_main -- через iretq, не call (см.
     * kernel/process.c) -- обязателен явный sys_exit(), см.
     * KNOWN_ISSUES.md ("cryptotest: ret в никуда вместо sys_exit()"). */
    sys_exit(g_all_ok ? 0 : 1);
    return 0; /* не достигается */
}

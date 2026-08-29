/* gpttest.c -- Фаза A плана UEFI_PLAN.md: собирает GPT (первичный +
 * резервный заголовок и таблицу разделов) в памяти, печатает сводку,
 * разбирает собранное обратно (gpt_parse()) и сверяет с тем, что было
 * задано -- НИ РАЗУ не трогая настоящий диск (никакого
 * sys_disk_read_raw/write_raw здесь нет и не должно быть). Тот же
 * принцип, что и у apps/mbrtest.c (см. INSTALLER_PLAN.md, Фаза 1) --
 * первый шаг перед тем, как вообще подпускать sys_disk_write_raw() к
 * делу для UEFI/GPT-установки (см. UEFI_PLAN.md, Фаза D).
 *
 * Формат уже независимо сверен с настоящим GPT-инструментом (`sgdisk`)
 * на этапе разработки -- см. общий комментарий в include/gpt.h и
 * UEFI_PLAN.md, Фаза A: "No problems found."
 *
 * Не требует ни одной capability -- вся работа в памяти. */

#include "mlapp.h"
#include "libc.h"
#include "gpt.h"

#define DEMO_ESP_SECTORS 204800u /* 100 МиБ ESP -- см. UEFI_PLAN.md, Фаза D */

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    u64 disk_sectors = (256ull * 1024ull * 1024ull) / GPT_SECTOR_SIZE; /* 256 МиБ по умолчанию */
    if (argc >= 2) disk_sectors = (u64)atoi(argv[1]);

    if (disk_sectors <= 34u + DEMO_ESP_SECTORS + 34u) {
        printf("gpttest: диск слишком маленький (%u секторов)\n", (u32)disk_sectors);
        sys_exit(1);
    }

    printf("=== gpttest -- сборка GPT в памяти (UEFI_PLAN.md, Фаза A) ===\n");
    printf("Диск: %u секторов (~%u МиБ)\n", (u32)disk_sectors, (u32)(disk_sectors / 2048u));

    /* ---- 1. Собираем два раздела: ESP + UFS на остаток диска ---- */
    gpt_partition_entry_t parts[GPT_MAX_PARTITIONS];
    memset(parts, 0, sizeof(parts));

    u64 esp_start = 34u;
    u64 esp_end = esp_start + DEMO_ESP_SECTORS - 1u;
    u64 ufs_start = esp_end + 1u;
    u64 ufs_end = disk_sectors - 34u - 1u;

    gpt_partition_entry_set(&parts[0], GPT_GUID_ESP, esp_start, esp_end, "ESP");
    gpt_partition_entry_set(&parts[1], GPT_GUID_MEETLOOK_UFS, ufs_start, ufs_end, "meetlook-ufs");

    printf("Раздел 0: ESP         LBA %u..%u (~%u МиБ)\n",
           (u32)esp_start, (u32)esp_end, (u32)((esp_end - esp_start + 1) / 2048u));
    printf("Раздел 1: meetlook-ufs LBA %u..%u (~%u МиБ)\n",
           (u32)ufs_start, (u32)ufs_end, (u32)((ufs_end - ufs_start + 1) / 2048u));

    /* ---- 2. disk_guid -- для демо берём фиксированное значение (не
     * случайное -- в этой ОС пока нет источника энтропии; install-os
     * в Фазе D возьмёт то же самое, что уже достаточно уникально в
     * пределах "одна демка -- один диск"). ---- */
    gpt_guid_t disk_guid = gpt_guid_from_fields(
        0xDEADBEEFu, 0xCAFEu, 0xF00Du,
        (const u8[8]){ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 });

    static u8 primary_header[GPT_SECTOR_SIZE];
    static u8 primary_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    static u8 backup_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    static u8 backup_header[GPT_SECTOR_SIZE];

    gpt_build(disk_sectors, disk_guid, parts, primary_header, primary_table, backup_table, backup_header);
    printf("\nGPT собран: primary header LBA 1, primary table LBA 2..%u, "
           "backup table LBA %u..%u, backup header LBA %u\n",
           1u + GPT_PARTITION_TABLE_SECTORS,
           (u32)(disk_sectors - 1u - GPT_PARTITION_TABLE_SECTORS), (u32)(disk_sectors - 2u),
           (u32)(disk_sectors - 1u));

    /* ---- 3. Разбираем обратно (и первичную, и резервную копию) и сверяем ---- */
    gpt_partition_entry_t parsed[GPT_MAX_PARTITIONS];
    if (!gpt_parse(primary_header, primary_table, parsed)) {
        printf("\ngpttest: FAIL -- gpt_parse(primary) не распознал собственный gpt_build()!\n");
        sys_exit(1);
    }

    gpt_partition_entry_t parsed_backup[GPT_MAX_PARTITIONS];
    if (!gpt_parse(backup_header, backup_table, parsed_backup)) {
        printf("\ngpttest: FAIL -- gpt_parse(backup) не распознал резервную копию!\n");
        sys_exit(1);
    }

    bool_t ok = TRUE;
    if (memcmp(&parsed[0], &parts[0], sizeof(gpt_partition_entry_t)) != 0) {
        printf("\ngpttest: FAIL -- parsed[0] (ESP) не совпадает с исходным\n");
        ok = FALSE;
    }
    if (memcmp(&parsed[1], &parts[1], sizeof(gpt_partition_entry_t)) != 0) {
        printf("\ngpttest: FAIL -- parsed[1] (UFS) не совпадает с исходным\n");
        ok = FALSE;
    }
    if (memcmp(parsed, parsed_backup, sizeof(parsed)) != 0) {
        printf("\ngpttest: FAIL -- резервная копия даёт другой набор разделов\n");
        ok = FALSE;
    }
    if (!gpt_partition_is_used(&parsed[0]) || !gpt_partition_is_used(&parsed[1])) {
        printf("\ngpttest: FAIL -- используемые разделы не распознаны как used\n");
        ok = FALSE;
    }
    for (u32 i = 2; i < GPT_MAX_PARTITIONS && ok; i++) {
        if (gpt_partition_is_used(&parsed[i])) {
            printf("\ngpttest: FAIL -- слот %u должен быть пуст\n", i);
            ok = FALSE;
        }
    }

    /* ---- 4. Порча одного байта -> gpt_parse обязан отказать (CRC32) ---- */
    u8 bad_header[GPT_SECTOR_SIZE];
    memcpy(bad_header, primary_header, GPT_SECTOR_SIZE);
    bad_header[50] ^= 0xFF;
    gpt_partition_entry_t dummy[GPT_MAX_PARTITIONS];
    if (gpt_parse(bad_header, primary_table, dummy)) {
        printf("\ngpttest: FAIL -- испорченный заголовок ПРИНЯТ (CRC32 не сработал)\n");
        ok = FALSE;
    }

    if (ok) {
        printf("\ngpttest: OK -- gpt_parse(gpt_build(...)) вернул то же самое для обеих копий,\n");
        printf("               испорченный CRC32 корректно отклонён.\n");
    }

    sys_exit(ok ? 0 : 1);
    return 0;
}

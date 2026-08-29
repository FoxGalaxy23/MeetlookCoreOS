/* mbrtest.c -- Фаза 1 плана инсталлятора (см. INSTALLER_PLAN.md):
 * собирает MBR-сектор в памяти (lib/mbr.c) с одной FAT32-партицией на
 * весь диск, печатает hex-дамп, затем разбирает собранный сектор
 * обратно (mbr_parse) и сверяет с тем, что было задано -- НИ РАЗУ не
 * трогая настоящий диск (никакого sys_disk_read_raw/write_raw здесь
 * нет и не должно быть). Это осознанно первый шаг перед тем, как
 * вообще подпускать sys_disk_write_raw() к делу -- см. общий
 * комментарий в include/mbr.h.
 *
 * Не требует ни одной capability -- вся работа в памяти. */

#include "mlapp.h"
#include "libc.h"
#include "mbr.h"

/* Раздел начинается с LBA 2048 (1 МиБ отступ -- см. INSTALLER_PLAN.md,
 * Фаза 1: стандартная схема BIOS+MBR, оставляет место в gap-области
 * под GRUB core.img между MBR и началом раздела). */
#define DEMO_PART_START_LBA 2048u

static void hexdump_sector(const u8 *buf, u32 len) {
    static const char hex[] = "0123456789abcdef";
    char line[16 * 3 + 1 + 32 + 1]; /* "xx "*16 + разделитель + ascii*16 + '\0' */

    for (u32 off = 0; off < len; off += 16) {
        u32 n = len - off < 16 ? len - off : 16;
        u32 pos = 0;

        for (u32 i = 0; i < 16; i++) {
            if (i < n) {
                u8 b = buf[off + i];
                line[pos++] = hex[b >> 4];
                line[pos++] = hex[b & 0xF];
            } else {
                line[pos++] = ' ';
                line[pos++] = ' ';
            }
            line[pos++] = ' ';
        }
        line[pos++] = '|';
        for (u32 i = 0; i < n; i++) {
            u8 b = buf[off + i];
            line[pos++] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        line[pos] = '\0';

        printf("  %04x  %s\n", off, line);
    }
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    /* Размер "диска" в секторах -- по умолчанию 256 МиБ (как у
     * test_target.img в INSTALLER_PLAN.md), либо из argv[1] для
     * ручных экспериментов. */
    u32 disk_sectors = (256u * 1024u * 1024u) / MBR_SECTOR_SIZE;
    if (argc >= 2) disk_sectors = (u32)atoi(argv[1]);

    if (disk_sectors <= DEMO_PART_START_LBA) {
        printf("mbrtest: диск слишком маленький (%u секторов, нужно > %u)\n",
               disk_sectors, DEMO_PART_START_LBA);
        sys_exit(1);
    }

    printf("=== mbrtest -- сборка MBR в памяти (INSTALLER_PLAN.md, Фаза 1) ===\n");
    printf("Диск: %u секторов (~%u МиБ)\n", disk_sectors, disk_sectors / 2048u);

    /* ---- 1. Собираем таблицу разделов: один FAT32 LBA-раздел на весь
     *    диск после отступа в 1 МиБ, помечен загрузочным. ---- */
    mbr_partition_t parts[MBR_PARTITION_COUNT];
    memset(parts, 0, sizeof(parts));

    u32 part_sectors = disk_sectors - DEMO_PART_START_LBA;
    mbr_partition_set(&parts[0], TRUE, MBR_PART_TYPE_FAT32_LBA, DEMO_PART_START_LBA, part_sectors);
    /* parts[1..3] остаются нулевыми -- пустые слоты, mbr_build() сам
     * запишет их как MBR_PART_TYPE_EMPTY/lba_start=0/sectors=0, т.к.
     * memset() их уже обнулил. */

    printf("Раздел 0: bootable=YES type=0x%x lba_start=%u sectors=%u (~%u МиБ)\n",
           parts[0].type, parts[0].lba_start, parts[0].sectors, parts[0].sectors / 2048u);

    /* ---- 2. Собираем сектор. boot_code -- пока заглушка (реальный
     *    boot.img от GRUB появится в Фазе 2 плана), просто узнаваемый
     *    паттерн 0x90 (x86 NOP), чтобы в hex-дампе было видно границу
     *    "код / таблица разделов / сигнатура". ---- */
    u8 fake_boot_code[MBR_BOOTCODE_SIZE];
    memset(fake_boot_code, 0x90, sizeof(fake_boot_code));

    mbr_sector_t sector;
    mbr_build(&sector, fake_boot_code, sizeof(fake_boot_code), parts);

    printf("\nСобранный сектор (%u байт), hex-дамп:\n", (u32)sizeof(sector));
    hexdump_sector((const u8 *)&sector, sizeof(sector));

    /* ---- 3. Разбираем собранное обратно и сверяем -- это то же
     *    самое, что должно произойти, когда install-os (Фаза 4) читает
     *    существующий MBR перед перезаписью, либо когда мы захотим
     *    прочитать разметку уже установленного диска. ---- */
    mbr_partition_t parsed[MBR_PARTITION_COUNT];
    if (!mbr_parse(&sector, parsed)) {
        printf("\nmbrtest: FAIL -- mbr_parse не распознал сигнатуру собственного mbr_build()!\n");
        sys_exit(1);
    }

    bool_t ok = TRUE;
    if (!mbr_partition_is_used(&parsed[0])) {
        printf("\nmbrtest: FAIL -- раздел 0 распознан как пустой\n");
        ok = FALSE;
    } else if (parsed[0].type != MBR_PART_TYPE_FAT32_LBA ||
               parsed[0].lba_start != DEMO_PART_START_LBA ||
               parsed[0].sectors != part_sectors ||
               parsed[0].boot_flag != MBR_PART_BOOTABLE) {
        printf("\nmbrtest: FAIL -- раздел 0 после mbr_parse не совпадает с исходным\n");
        ok = FALSE;
    }
    for (u32 i = 1; i < MBR_PARTITION_COUNT && ok; i++) {
        if (mbr_partition_is_used(&parsed[i])) {
            printf("\nmbrtest: FAIL -- слот %u должен быть пуст, а не пуст\n", i);
            ok = FALSE;
        }
    }

    if (ok) {
        printf("\nmbrtest: OK -- mbr_parse(mbr_build(...)) вернул то же самое, что было задано.\n");
    }

    sys_exit(ok ? 0 : 1);
    return 0;
}

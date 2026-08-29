/* update-os.c -- ATOMIC_PLAN.md, Фаза 7: обновление уже установленной
 * Atomic-системы (см. apps/install-os.c, режим "Atomic", Фазы 2/6).
 * Пишет ТЕКУЩИЙ (уже собранный, в котором работает этот самый
 * инстанс update-os -- см. общий комментарий в install-os.c про
 * самоссылающийся kernel.flat, тот же принцип) kernel.flat в
 * НЕАКТИВНЫЙ слот целиком, СВЕРЯЕТ запись через CRC32 и, только если
 * сверка прошла, атомарно переключает boot-meta -- активным становится
 * только что записанный слот, с BOOTMETA_DEFAULT_TRIES попытками и
 * boot_confirmed=0 (см. общий комментарий у bootmeta_mark_confirmed(),
 * include/bootmeta.h, и у SYS_BOOT_CONFIRM, include/sysabi.h -- их уже
 * подтверждает само ядро при следующей успешной загрузке, boot/stage2_atomic.s
 * уже умеет decrement/rollback, если что-то пойдёт не так -- ATOMIC_PLAN.md,
 * Фаза 6, полностью готова и проверена живьём в QEMU).
 *
 * ГЛАВНАЯ ГАРАНТИЯ этого файла, ради которой вся Фаза 7 вообще
 * существует: boot-meta переключается ТОЛЬКО ПОСЛЕ успешной проверки
 * записи по CRC32 -- если запись не влезла, диск отказал посреди записи,
 * или прочитанное с диска не совпало с тем, что должно было записаться,
 * update-os останавливается, НЕ трогая boot-meta вообще -- старый,
 * заведомо рабочий слот остаётся активным, система как ни в чём не
 * бывало загрузится с него на следующей перезагрузке. Единственная
 * операция, которая реально переключает систему на новый слот -- запись
 * ОДНОГО сектора boot-meta (bootmeta_serialize() + sys_disk_write_raw()
 * на 1 сектор) -- она либо целиком происходит, либо (при потере питания
 * посередине) целиком не происходит: сектор либо старый, либо новый,
 * никакого промежуточного состояния на уровне одного сектора не бывает
 * (тот же аргумент, которым ATOMIC_PLAN.md обосновывает всю схему).
 *
 * Требует disk_raw (SYS_DISK_READ_RAW/WRITE_RAW, SYS_BOOT_ASSET_WRITE_TO_DISK/
 * VERIFY_ON_DISK) -- та же capability и тот же кернел-side гейт
 * (users_current_is_root(), см. общий комментарий у CAP_DISK_RAW,
 * include/container.h), что у install-os/create-user -- НЕ "доверие
 * бинарнику", а необходимое условие, при котором ядро вообще что-то
 * позволит. Проверка root ниже (sys_users_whoami) ДУБЛИРУЕТ кернел-side
 * гейт нарочно, тем же духом, что apps/create-user.c -- см. её общий
 * комментарий за тем же обоснованием. */

#include "mlapp.h"
#include "libc.h"
#include "usyscall.h"
#include "gpt.h"
#include "bootmeta.h"
#include "boot_assets.h"

static bool_t guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== update-os -- обновление активного слота (ATOMIC_PLAN.md, Фаза 7) ===\n\n");

    /* ---- 0. root? -- см. общий комментарий в начале файла. ---- */
    sys_user_info_t me;
    if (sys_users_whoami(&me) < 0) {
        printf("update-os: не удалось узнать текущего пользователя (sys_users_whoami)\n");
        sys_exit(1);
    }
    if (!me.is_root) {
        printf("update-os: нужны права root (сейчас: %s) -- войдите под root ('su' или перелогиньтесь)\n", me.name);
        sys_exit(1);
    }

    /* ---- 1. Выбор диска -- та же UX, что install-os/create-user. ---- */
    sys_storage_info_t disks[16];
    u32 disk_count = 0;
    if (sys_storage_list(disks, 16, &disk_count) < 0 || disk_count == 0) {
        printf("update-os: не нашёл ни одного диска (sys_storage_list)\n");
        sys_exit(1);
    }
    for (u32 i = 0; i < disk_count; i++) {
        u32 mb = disks[i].sectors / 2048u;
        printf("  [%u] %s -- %s, %u МиБ (%u секторов)\n",
               i, disks[i].location, disks[i].model, mb, disks[i].sectors);
    }
    printf("\nНомер диска (уже установленная Atomic-система): ");
    char line[128];
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    s32 drive_s = atoi(line);
    if (drive_s < 0 || (u32)drive_s >= disk_count) {
        printf("update-os: неверный номер диска\n");
        sys_exit(1);
    }
    u32 drive = (u32)drive_s;

    /* ---- 2. Читаем GPT, ищем boot-meta и ОБА os-slot -- тот же приём,
     * что apps/create-user.c (см. её общий комментарий за user-images) --
     * поиск по type GUID через ВСЮ таблицу, не по фиксированному индексу:
     * os-slot-a/os-slot-b ДЕЛЯТ один и тот же type GUID
     * (GPT_GUID_MEETLOOK_OS_SLOT, include/gpt.h) -- различаем их порядком
     * появления в таблице (gpt_parse() сохраняет позиционный порядок
     * записи на диске без сортировки) -- install-os (apps/install-os.c,
     * install_atomic_layout()) ВСЕГДА пишет slot-a раньше slot-b в
     * таблице, поэтому первое совпадение -- всегда A, второе -- всегда
     * B. ---- */
    u8 header_sector[GPT_SECTOR_SIZE];
    static u8 table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    if (sys_disk_read_raw(drive, 1u, 1u, header_sector) < 0 ||
        sys_disk_read_raw(drive, 2u, GPT_PARTITION_TABLE_SECTORS, table) < 0) {
        printf("update-os: не удалось прочитать GPT с диска [%u]\n", drive);
        sys_exit(1);
    }

    gpt_partition_entry_t parts[GPT_MAX_PARTITIONS];
    if (!gpt_parse(header_sector, table, parts)) {
        printf("update-os: диск [%u] не размечен под GPT (или разметка битая) --\n", drive);
        printf("это должна быть уже установленная Atomic-система (install-os, режим \"Atomic\")\n");
        sys_exit(1);
    }

    s32 bootmeta_index = -1;
    s32 slot_a_index = -1;
    s32 slot_b_index = -1;
    for (u32 i = 0; i < GPT_MAX_PARTITIONS; i++) {
        if (!gpt_partition_is_used(&parts[i])) continue;
        if (bootmeta_index < 0 && guid_equal(&parts[i].partition_type_guid, &GPT_GUID_MEETLOOK_BOOTMETA)) {
            bootmeta_index = (s32)i;
        } else if (guid_equal(&parts[i].partition_type_guid, &GPT_GUID_MEETLOOK_OS_SLOT)) {
            if (slot_a_index < 0) slot_a_index = (s32)i;
            else if (slot_b_index < 0) slot_b_index = (s32)i;
        }
    }
    if (bootmeta_index < 0 || slot_a_index < 0 || slot_b_index < 0) {
        printf("update-os: на диске [%u] не нашлись все три раздела (boot-meta + os-slot-a + os-slot-b) --\n", drive);
        printf("это должна быть уже установленная Atomic-система (install-os, режим \"Atomic\")\n");
        sys_exit(1);
    }

    u32 bootmeta_lba = (u32)parts[bootmeta_index].starting_lba;
    u32 slot_lba[2]; /* индекс -- BOOTMETA_SLOT_A/B, см. include/bootmeta.h */
    u32 slot_sectors[2];
    slot_lba[BOOTMETA_SLOT_A] = (u32)parts[slot_a_index].starting_lba;
    slot_sectors[BOOTMETA_SLOT_A] = (u32)(parts[slot_a_index].ending_lba - parts[slot_a_index].starting_lba + 1u);
    slot_lba[BOOTMETA_SLOT_B] = (u32)parts[slot_b_index].starting_lba;
    slot_sectors[BOOTMETA_SLOT_B] = (u32)(parts[slot_b_index].ending_lba - parts[slot_b_index].starting_lba + 1u);

    /* ---- 3. Читаем и проверяем boot-meta -- узнаём текущий active_slot. ---- */
    u8 bootmeta_sector[BOOTMETA_SECTOR_SIZE];
    if (sys_disk_read_raw(drive, bootmeta_lba, 1u, bootmeta_sector) < 0) {
        printf("update-os: не удалось прочитать boot-meta (LBA %u) с диска [%u]\n", bootmeta_lba, drive);
        sys_exit(1);
    }
    bootmeta_t bm;
    if (!bootmeta_parse(bootmeta_sector, &bm)) {
        printf("update-os: boot-meta на диске [%u] повреждена или отсутствует (magic/CRC32 не сошлись) --\n", drive);
        printf("используйте install-os заново, если диск действительно новый/пустой\n");
        sys_exit(1);
    }

    u8 target_slot = bootmeta_other_slot(bm.active_slot);
    u32 target_lba = slot_lba[target_slot];
    u32 target_sectors = slot_sectors[target_slot];

    printf("\nТекущий активный слот: %s\n", bm.active_slot == BOOTMETA_SLOT_A ? "A" : "B");
    printf("Буду писать НОВЫЙ kernel.flat (этой же самой системы, из которой запущен\n");
    printf("update-os -- см. общий комментарий в начале файла) в НЕАКТИВНЫЙ слот %s\n",
           target_slot == BOOTMETA_SLOT_A ? "A" : "B");
    printf("(LBA %u..%u, %u секторов).\n", target_lba, target_lba + target_sectors - 1u, target_sectors);
    printf("Слот %s (текущий активный) НЕ ТРОГАЕТСЯ -- если что-то пойдёт не так,\n",
           bm.active_slot == BOOTMETA_SLOT_A ? "A" : "B");
    printf("система как ни в чём не бывало продолжит грузиться с него.\n\n");
    printf("Введите заглавными буквами \"YES\" для подтверждения: ");
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    if (strcmp(line, "YES") != 0) {
        printf("update-os: подтверждение не получено, отмена.\n");
        sys_exit(1);
    }

    /* ---- 4. Пишем kernel.flat в неактивный слот -- напрямую между
     * .rodata ядра и диском (SYS_BOOT_ASSET_WRITE_TO_DISK), см. общий
     * комментарий у install_kernel_region(), apps/install-os.c, за тем,
     * почему НЕ через промежуточный userspace-буфер. ---- */
    printf("\nПишу kernel.flat в слот %s...\n", target_slot == BOOTMETA_SLOT_A ? "A" : "B");
    s32 real_size = sys_boot_asset_write_to_disk(BOOT_ASSET_KERNEL_FLAT, drive, target_lba, target_sectors);
    if (real_size < 0) {
        printf("update-os: ошибка записи kernel.flat (не влезает в слот, либо ошибка диска) -- ОСТАНОВЛЕНО\n");
        printf("boot-meta НЕ тронута -- слот %s остаётся активным, система загрузится как раньше.\n",
               bm.active_slot == BOOTMETA_SLOT_A ? "A" : "B");
        sys_exit(1);
    }
    printf("Записано %d байт. Сверяю через CRC32...\n", real_size);

    /* ---- 5. Проверка -- ЦЕНТРАЛЬНЫЙ шаг всей Фазы 7, см. общий
     * комментарий в начале файла. Если не совпало -- boot-meta НЕ
     * трогаем, выходим с ошибкой. ---- */
    if (sys_boot_asset_verify_on_disk(BOOT_ASSET_KERNEL_FLAT, drive, target_lba) < 0) {
        printf("update-os: kernel.flat в слоте %s НЕ совпал по CRC32 после записи -- ОСТАНОВЛЕНО\n",
               target_slot == BOOTMETA_SLOT_A ? "A" : "B");
        printf("boot-meta НЕ тронута -- слот %s остаётся активным, система загрузится как раньше.\n",
               bm.active_slot == BOOTMETA_SLOT_A ? "A" : "B");
        printf("Слот %s теперь содержит битые данные -- НЕ страшно (он неактивен), но следующий\n",
               target_slot == BOOTMETA_SLOT_A ? "A" : "B");
        printf("запуск update-os перезапишет его заново.\n");
        sys_exit(1);
    }
    printf("Запись проверена -- совпадает byte в byte.\n");

    /* ---- 6. Атомарное переключение -- ОДИН сектор, см. общий
     * комментарий в начале файла за тем, почему именно это делает
     * переключение атомарным. boot_tries_left=BOOTMETA_DEFAULT_TRIES,
     * boot_confirmed=0 -- см. общий комментарий у BOOTMETA_DEFAULT_TRIES/
     * bootmeta_mark_confirmed(), include/bootmeta.h: у нового слота есть
     * несколько попыток догрузиться и самоподтвердиться (SYS_BOOT_CONFIRM,
     * kernel/kernel.c, уже вызывается ядром автоматически после успешного
     * входа в систему), прежде чем boot/stage2_atomic.s (ATOMIC_PLAN.md,
     * Фаза 6) автоматически откатится обратно на слот %s. ---- */
    bootmeta_t new_bm;
    new_bm.magic = BOOTMETA_MAGIC;
    new_bm.active_slot = target_slot;
    new_bm.boot_tries_left = (u8)BOOTMETA_DEFAULT_TRIES;
    new_bm.boot_confirmed = 0;
    new_bm.reserved = 0;

    u8 new_bootmeta_sector[BOOTMETA_SECTOR_SIZE];
    bootmeta_serialize(&new_bm, new_bootmeta_sector);

    printf("Переключаю boot-meta (LBA %u) на слот %s...\n", bootmeta_lba, target_slot == BOOTMETA_SLOT_A ? "A" : "B");
    if (sys_disk_write_raw(drive, bootmeta_lba, 1u, new_bootmeta_sector) < 0) {
        printf("update-os: ошибка записи boot-meta -- ОСТАНОВЛЕНО В ХУДШЕЙ ТОЧКЕ\n");
        printf("kernel.flat в слоте %s записан и проверен, но boot-meta могла не переключиться --\n",
               target_slot == BOOTMETA_SLOT_A ? "A" : "B");
        printf("перечитайте её (например, 'runc /containers/atomictest.manifest' или повторно\n");
        printf("запустите update-os) прежде чем перезагружаться.\n");
        sys_exit(1);
    }

    printf("\n=== Готово. Активный слот теперь %s (%u попыток, не подтверждено). ===\n",
           target_slot == BOOTMETA_SLOT_A ? "A" : "B", (unsigned)BOOTMETA_DEFAULT_TRIES);
    printf("Перезагрузитесь -- boot/stage2_atomic.s (ATOMIC_PLAN.md, Фаза 6) загрузит\n");
    printf("новый слот; после успешного входа в систему ядро само подтвердит загрузку\n");
    printf("(SYS_BOOT_CONFIRM). Если что-то пойдёт не так -- через %u неудачных попыток\n", (unsigned)BOOTMETA_DEFAULT_TRIES);
    printf("произойдёт автооткат на прежний слот %s -- без вмешательства человека.\n",
           bm.active_slot == BOOTMETA_SLOT_A ? "A" : "B");

    sys_exit(0);
    return 0;
}

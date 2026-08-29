#include "userdisk.h"
#include "storage.h"
#include "gpt.h"
#include "vfs.h"
#include "string.h"

/* См. include/userdisk.h. */

static bool_t guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

/* Большие буферы -- НАРОЧНО static (BSS), не на стеке: одна GPT-таблица
 * разделов -- это GPT_PARTITION_TABLE_SECTORS*GPT_SECTOR_SIZE = 16384
 * байт, а массив разобранных записей (gpt_partition_entry_t[128]) --
 * ещё 16384 -- вместе они одни забили бы весь TASK_STACK_SIZE (16 КиБ,
 * include/task.h), на котором выполняется kernel_main()/users_login_prompt()
 * после tasking_init(). Тот же приём, что apps/create-user.c (там --
 * по той же причине, но там это ограничение компилятора userspace-стека,
 * здесь -- kernel-side, но причина идентична). Не реентерабельно (как и
 * все остальные подобные static-буферы в этом дереве, например
 * kernel/loader.c) -- нормально: сканирование дисков однопоточное,
 * вызывается только из users_login_prompt() на пути входа в систему. */
static u8 s_gpt_header[GPT_SECTOR_SIZE];
static u8 s_gpt_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
static gpt_partition_entry_t s_gpt_parts[GPT_MAX_PARTITIONS];
static u8 s_slot_sector[USERIMG_SECTOR_SIZE];

/* Ищет раздел user-images на ОДНОМ диске. TRUE + заполняет
 * out_pool_start_lba и out_num_slots, если раздел найден и на нём есть
 * место хотя бы под один слот -- FALSE, если диск слишком мал, не
 * GPT-размечен (gpt_parse() отклонил -- пустой/MBR-only/мусорный
 * диск, это НЕ ошибка), либо GPT есть, но раздела user-images в ней
 * нет (диск размечен НЕ через install-os "Atomic", а по-старому). */
static bool_t find_user_images_partition(u32 drive_index, u64 *out_pool_start_lba, u32 *out_num_slots) {
    storage_dev_t *d = storage_get_drive(drive_index);
    if (!d || !d->present) return FALSE;
    /* Меньше, чем protective MBR (1) + первичный заголовок (1) +
       первичная таблица (32) -- заведомо не может нести GPT с разделом. */
    if (d->sectors < 1u + GPT_PARTITION_TABLE_SECTORS + 1u) return FALSE;

    if (!storage_read_sectors(drive_index, 1u, 1u, s_gpt_header)) return FALSE;
    if (!storage_read_sectors(drive_index, 2u, (u8)GPT_PARTITION_TABLE_SECTORS, s_gpt_table)) return FALSE;

    if (!gpt_parse(s_gpt_header, s_gpt_table, s_gpt_parts)) return FALSE; /* не GPT-диск -- обычный случай, не ошибка */

    for (u32 i = 0; i < GPT_MAX_PARTITIONS; i++) {
        if (gpt_partition_is_used(&s_gpt_parts[i]) &&
            guid_equal(&s_gpt_parts[i].partition_type_guid, &GPT_GUID_MEETLOOK_USER_IMAGES)) {
            u64 start = s_gpt_parts[i].starting_lba;
            u64 end = s_gpt_parts[i].ending_lba;
            u32 slots = (u32)((end - start + 1u) / USERIMG_SLOT_SECTORS);
            if (slots == 0) return FALSE; /* раздел есть, но меньше одного слота -- ничего не сканировать */
            *out_pool_start_lba = start;
            *out_num_slots = slots;
            return TRUE;
        }
    }
    return FALSE; /* GPT валиден, но user-images в ней нет -- диск размечен не "Atomic" */
}

/* username[] в заголовке НЕ обязательно NUL-terminated (см.
 * include/userimg.h) -- копирует ровно до первого нуля либо до
 * USERIMG_USERNAME_LEN, гарантируя NUL-terminated результат в out
 * (буфер размера USERIMG_USERNAME_LEN+1, как в userdisk_entry_t). */
static void copy_username(const char raw[USERIMG_USERNAME_LEN], char out[USERIMG_USERNAME_LEN + 1]) {
    u32 len = 0;
    while (len < USERIMG_USERNAME_LEN && raw[len] != '\0') len++;
    memcpy(out, raw, len);
    out[len] = '\0';
}

u32 userdisk_scan(userdisk_entry_t out[USERDISK_MAX_ENTRIES]) {
    u32 found = 0;
    u32 drive_count = storage_drive_count();

    for (u32 drive = 0; drive < drive_count && found < USERDISK_MAX_ENTRIES; drive++) {
        u64 pool_start = 0;
        u32 num_slots = 0;
        if (!find_user_images_partition(drive, &pool_start, &num_slots)) continue;

        for (u32 i = 0; i < num_slots && found < USERDISK_MAX_ENTRIES; i++) {
            u64 slot_lba = pool_start + (u64)i * USERIMG_SLOT_SECTORS;
            if (!storage_read_sectors(drive, (u32)slot_lba, 1u, s_slot_sector)) continue;

            userimg_header_t hdr;
            if (!userimg_header_parse(s_slot_sector, &hdr)) continue; /* слот свободен -- пропускаем, см. userimg.h */

            copy_username(hdr.username, out[found].username);
            out[found].drive_index = drive;
            out[found].slot_header_lba = (u32)slot_lba;
            found++;
        }
    }
    return found;
}

bool_t userdisk_login(const userdisk_entry_t *entry, const char *password, u32 password_len) {
    /* Перечитываем заголовок заново (не полагаемся на память со времени
       userdisk_scan()) -- диск мог измениться (переустановка, повторный
       create-user), а сам заголовок -- всего один сектор, лишнее чтение
       ничего не стоит. */
    if (!storage_read_sectors(entry->drive_index, entry->slot_header_lba, 1u, s_slot_sector)) return FALSE;

    userimg_header_t hdr;
    if (!userimg_header_parse(s_slot_sector, &hdr)) return FALSE; /* слот пропал/испорчен со времени сканирования */

    u8 master_key[USERIMG_MASTER_KEY_LEN];
    bool_t ok = userimg_verify_password(&hdr, password, password_len, master_key);
    if (!ok) {
        return FALSE; /* userimg_verify_password() сама не отличает "неверный пароль" от "битый заголовок" -- см. userimg.h */
    }

    /* Тело слота начинается сразу ПОСЛЕ заголовка -- ровно тот же
       расчёт, что apps/create-user.c при форматировании (Фаза 3/4):
       один сектор под заголовок, остаток слота -- под UFS+AES-XTS. */
    u32 slot_body_lba_start = entry->slot_header_lba + 1u;
    bool_t mounted = fs_mount_home_encrypted(entry->drive_index, slot_body_lba_start, master_key, FALSE);

    memset(master_key, 0, sizeof(master_key)); /* best-effort стереть со стека, см. общий комментарий в userimg.c */
    return mounted;
}

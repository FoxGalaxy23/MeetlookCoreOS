/* create-user.c -- ATOMIC_PLAN.md, Фаза 3+4: пишет заголовок НОВОГО
 * зашифрованного пользовательского образа (include/userimg.h) в
 * свободный слот раздела user-images атомарно размеченного диска
 * (ATOMIC_PLAN.md, Фаза 2 -- см. apps/install-os.c, режим "Atomic"),
 * и форматирует ТЕЛО этого слота (обычный UFS поверх AES-XTS, Фаза 4)
 * через SYS_FS_FORMAT_HOME_ENCRYPTED -- см. общий комментарий у неё,
 * include/sysabi.h, и у ufs_format_encrypted(), fs/ufs.c (сама
 * форматирующая логика живёт в ядре, этот файл -- только вызывающая
 * сторона, с уже готовым master_key на руках).
 *
 * Требует disk_raw (SYS_DISK_READ_RAW/WRITE_RAW, SYS_FS_FORMAT_HOME_ENCRYPTED)
 * -- ЕДИНСТВЕННЫЙ способ добраться до сырых секторов пула user-images,
 * до какой-либо файловой системы внутри слота. ВАЖНО -- это НЕ install-os
 * (доверенный, однократный, ставит всю систему на чистый диск): это
 * повторяемая, обычная административная операция ("завести ещё одного
 * пользователя на уже работающей системе"), поэтому disk_raw ей выдан
 * по ДРУГОЙ причине и под ДРУГИМ условием, не "install-os -- особый
 * бинарник" -- см. общий комментарий у CAP_DISK_RAW (include/container.h):
 * ядро само требует, чтобы ТЕКУЩИЙ вошедший пользователь (kernel/users.c,
 * НЕ путать с userimg.h -- это два РАЗНЫХ, независимых понятия
 * "пользователь": одно -- модель прав шелла/ядра уровня ОС, другое --
 * зашифрованный per-user образ на диске, который эта самая программа и
 * создаёт) был root, прежде чем вообще пропустить SYS_DISK_READ_RAW/
 * WRITE_RAW -- этот бинарник может обладать capability и всё равно
 * ничего не смочь, если сессия не root. Проверка ниже (users_whoami)
 * ДУБЛИРУЕТ кернел-side гейт нарочно -- не как единственная защита
 * (ей и не нужно быть), а чтобы дать пользователю понятное сообщение
 * СРАЗУ, а не невнятную ошибку прав после половины диалога. */

#include "mlapp.h"
#include "libc.h"
#include "usyscall.h"
#include "gpt.h"
#include "userimg.h"

static bool_t guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

/* username -- ТЕ ЖЕ правила, что kernel/users.c::name_is_valid() (не
 * переиспользованы напрямую -- та функция static в ядре, не в ABI):
 * непусто, короче USERIMG_USERNAME_LEN, только [a-zA-Z0-9_-]. Это имя
 * ВНУТРИ зашифрованного образа (userimg.h), НЕ обязано (пока, до Фазы
 * 5) совпадать с именем в kernel/users.c -- но одинаковые правила
 * снижают путаницу. */
static bool_t username_is_valid(const char *name) {
    u32 len = strlen(name);
    if (len == 0 || len >= USERIMG_USERNAME_LEN) return FALSE;
    for (u32 i = 0; i < len; i++) {
        char c = name[i];
        bool_t ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return FALSE;
    }
    return TRUE;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== create-user -- новый зашифрованный пользовательский образ (ATOMIC_PLAN.md, Фаза 3) ===\n\n");

    /* ---- 0. root? -- см. общий комментарий в начале файла: это
     * дубль кернел-side проверки, не замена ей. ---- */
    sys_user_info_t me;
    if (sys_users_whoami(&me) < 0) {
        printf("create-user: не удалось узнать текущего пользователя (sys_users_whoami)\n");
        sys_exit(1);
    }
    if (!me.is_root) {
        printf("create-user: нужны права root (сейчас: %s) -- войдите под root ('su' или перелогиньтесь)\n", me.name);
        sys_exit(1);
    }

    /* ---- 1. Выбор диска -- та же UX, что apps/install-os.c. ---- */
    sys_storage_info_t disks[16];
    u32 disk_count = 0;
    if (sys_storage_list(disks, 16, &disk_count) < 0 || disk_count == 0) {
        printf("create-user: не нашёл ни одного диска (sys_storage_list)\n");
        sys_exit(1);
    }
    for (u32 i = 0; i < disk_count; i++) {
        u32 mb = disks[i].sectors / 2048u;
        printf("  [%u] %s -- %s, %u МиБ (%u секторов)\n",
               i, disks[i].location, disks[i].model, mb, disks[i].sectors);
    }
    printf("\nНомер диска: ");
    char line[128];
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    s32 drive_s = atoi(line);
    if (drive_s < 0 || (u32)drive_s >= disk_count) {
        printf("create-user: неверный номер диска\n");
        sys_exit(1);
    }
    u32 drive = (u32)drive_s;

    /* ---- 2. Читаем GPT, ищем раздел user-images -- см. include/gpt.h
     * за общим комментарием у GPT_GUID_MEETLOOK_USER_IMAGES. ---- */
    u8 header_sector[GPT_SECTOR_SIZE];
    static u8 table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE];
    if (sys_disk_read_raw(drive, 1u, 1u, header_sector) < 0 ||
        sys_disk_read_raw(drive, 2u, GPT_PARTITION_TABLE_SECTORS, table) < 0) {
        printf("create-user: не удалось прочитать GPT с диска [%u]\n", drive);
        sys_exit(1);
    }

    gpt_partition_entry_t parts[GPT_MAX_PARTITIONS];
    if (!gpt_parse(header_sector, table, parts)) {
        printf("create-user: диск [%u] не размечен под GPT (или разметка битая) --\n", drive);
        printf("используйте install-os, режим \"Atomic\" (ATOMIC_PLAN.md, Фаза 2)\n");
        sys_exit(1);
    }

    s32 pool_index = -1;
    for (u32 i = 0; i < GPT_MAX_PARTITIONS; i++) {
        if (gpt_partition_is_used(&parts[i]) && guid_equal(&parts[i].partition_type_guid, &GPT_GUID_MEETLOOK_USER_IMAGES)) {
            pool_index = (s32)i;
            break;
        }
    }
    if (pool_index < 0) {
        printf("create-user: на диске [%u] нет раздела user-images -- используйте install-os, режим \"Atomic\"\n", drive);
        sys_exit(1);
    }
    u64 pool_start = parts[pool_index].starting_lba;
    u64 pool_end = parts[pool_index].ending_lba;
    u32 num_slots = (u32)((pool_end - pool_start + 1u) / USERIMG_SLOT_SECTORS);
    if (num_slots == 0) {
        printf("create-user: раздел user-images слишком мал даже для одного слота\n");
        sys_exit(1);
    }
    printf("user-images: LBA %u..%u, %u слотов по %u МиБ\n",
           (u32)pool_start, (u32)pool_end, num_slots, USERIMG_SLOT_SECTORS / 2048u);

    /* ---- 3. Имя нового пользователя -- заодно сканируем ВСЕ слоты:
     * ищем и занятое ли уже это имя, и первый свободный слот (magic не
     * совпал, см. userimg_header_parse()) -- см. общий комментарий в
     * include/userimg.h за тем, почему magic-only парсинг достаточен,
     * чтобы отличить "слот занят" от "слот свободен" без пароля. ---- */
    printf("\nИмя нового пользователя: ");
    if (readline(line, sizeof(line)) < 0) sys_exit(1);
    if (!username_is_valid(line)) {
        printf("create-user: имя должно быть непустым, короче %u байт, только [a-zA-Z0-9_-]\n", USERIMG_USERNAME_LEN);
        sys_exit(1);
    }
    static char username[USERIMG_USERNAME_LEN + 1];
    strncpy(username, line, USERIMG_USERNAME_LEN);
    username[USERIMG_USERNAME_LEN] = '\0';
    u32 username_len = strlen(username);

    s32 free_slot = -1;
    static u8 slot_sector[USERIMG_SECTOR_SIZE];
    for (u32 i = 0; i < num_slots; i++) {
        u64 slot_lba = pool_start + (u64)i * USERIMG_SLOT_SECTORS;
        if (sys_disk_read_raw(drive, (u32)slot_lba, 1u, slot_sector) < 0) {
            printf("create-user: ошибка чтения слота %u -- ОСТАНОВЛЕНО\n", i);
            sys_exit(1);
        }
        userimg_header_t existing;
        if (userimg_header_parse(slot_sector, &existing)) {
            /* Слот занят -- сверяем имя (сравнение по USERIMG_USERNAME_LEN
             * байт, поле НЕ обязательно NUL-terminated -- см. include/userimg.h). */
            if (memcmp(existing.username, username, username_len) == 0 &&
                (username_len == USERIMG_USERNAME_LEN || existing.username[username_len] == '\0')) {
                printf("create-user: пользователь \"%s\" уже существует (слот %u)\n", username, i);
                sys_exit(1);
            }
        } else if (free_slot < 0) {
            free_slot = (s32)i; /* первый свободный -- запоминаем, но сканирование НЕ прерываем (нужно доскрести все занятые ради проверки имени выше) */
        }
    }
    if (free_slot < 0) {
        printf("create-user: пул user-images заполнен (%u/%u слотов занято)\n", num_slots, num_slots);
        sys_exit(1);
    }
    u64 target_lba = pool_start + (u64)free_slot * USERIMG_SLOT_SECTORS;
    printf("Слот %d свободен (LBA %u) -- сюда запишется новый образ.\n", free_slot, (u32)target_lba);

    /* ---- 4. Пароль -- дважды, с маскировкой (sys_users_read_password(),
     * та же самая функция ввода, что использует экран входа kernel/users.c
     * -- НЕ обычный readline(), пароль на экране не отображается).
     * ВАЖНО -- это ограничивает пароль зашифрованного образа (userimg.h)
     * длиной USER_PASS_MAX (include/users.h, 32 байта, включая '\0'):
     * sys_users_read_password() сама клампит maxlen до USER_PASS_MAX
     * внутри ядра (kernel/syscall.c) НЕЗАВИСИМО от размера буфера здесь
     * -- больший буфер ничего не даёт. Отдельного, не завязанного на
     * kernel/users.c способа ввести длинный пароль с маскировкой сейчас
     * нет; для PBKDF2 длина пароля не принципиальна, но 31 символ --
     * заметное ограничение, отметить как известное ограничение, а не
     * молча смириться. */
#define CREATEUSER_PASSWORD_MAX 32u
    static char password[CREATEUSER_PASSWORD_MAX];
    static char password2[CREATEUSER_PASSWORD_MAX];
    printf("Пароль: ");
    s32 pass_len = sys_users_read_password(password, sizeof(password));
    if (pass_len < 0) sys_exit(1);
    if (pass_len == 0) {
        printf("create-user: пустой пароль не годится\n");
        sys_exit(1);
    }
    printf("Повторите пароль: ");
    s32 pass2_len = sys_users_read_password(password2, sizeof(password2));
    if (pass2_len < 0) sys_exit(1);
    if (pass_len != pass2_len || memcmp(password, password2, (u32)pass_len) != 0) {
        printf("create-user: пароли не совпадают -- ОСТАНОВЛЕНО\n");
        sys_exit(1);
    }

    /* ---- 5. Создание заголовка + запись в слот ---- */
    userimg_header_t hdr;
    u8 master_key[USERIMG_MASTER_KEY_LEN];
    bool_t created = userimg_header_create(&hdr, username, username_len,
                                            password, (u32)pass_len,
                                            USERIMG_DEFAULT_KDF_ITERATIONS,
                                            master_key);
    memset(password, 0, sizeof(password));   /* пароль в памяти больше не нужен -- best-effort стереть */
    memset(password2, 0, sizeof(password2));
    if (!created) {
        memset(master_key, 0, sizeof(master_key));
        printf("create-user: RDRAND недоступен на этом CPU -- без него новый образ не создать\n");
        sys_exit(1);
    }

    static u8 out_sector[USERIMG_SECTOR_SIZE];
    userimg_header_serialize(&hdr, out_sector);
    if (sys_disk_write_raw(drive, (u32)target_lba, 1u, out_sector) < 0) {
        memset(master_key, 0, sizeof(master_key));
        printf("create-user: ошибка записи заголовка в слот -- ОСТАНОВЛЕНО\n");
        sys_exit(1);
    }

    /* ---- 6. Форматирование тела (ATOMIC_PLAN.md, Фаза 4, хвост) --
     * master_key ещё в памяти именно для этого -- см. общий комментарий
     * в apps/create-user.c начала файла. Тело -- весь слот МИНУС один
     * сектор под уже записанный выше заголовок (см. include/userimg.h
     * за USERIMG_SLOT_SECTORS). Если это упадёт -- заголовок уже на
     * диске, но пользователь ещё непригоден к использованию (magic
     * там есть, но внутри не UFS, а мусор/нули) -- честно сказано в
     * сообщении об ошибке, не выдаём частичный успех за полный. */
    u32 slot_body_lba_start = (u32)target_lba + 1u;
    u32 slot_body_sectors = USERIMG_SLOT_SECTORS - 1u;
    bool_t formatted = sys_fs_format_home_encrypted(drive, slot_body_lba_start, slot_body_sectors, master_key) >= 0;
    memset(master_key, 0, sizeof(master_key)); /* дальше не нужен -- ядро само получило свою копию через syscall */
    if (!formatted) {
        printf("create-user: заголовок записан, но форматирование тела не удалось -- слот %d частично инициализирован, пользователем \"%s\" воспользоваться нельзя\n", free_slot, username);
        sys_exit(1);
    }

    printf("\n=== create-user: пользователь \"%s\" создан (слот %d, LBA %u) ===\n",
           username, free_slot, (u32)target_lba);
    printf("Заголовок и тело (пустой UFS поверх AES-XTS) записаны и отформатированы.\n");
    printf("Войти в этот образ пока нельзя -- ждёт сам вход в систему (ATOMIC_PLAN.md, Фаза 5),\n");
    printf("который умел бы проверить пароль и смонтировать fs_mount_home_encrypted().\n");

    sys_exit(0);
    return 0; /* не достигается */
}

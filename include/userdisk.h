#ifndef USERDISK_H
#define USERDISK_H

#include "types.h"
#include "userimg.h"

/* userdisk.h -- ATOMIC_PLAN.md, Фаза 5: сканирование раздела
 * user-images (GPT_GUID_MEETLOOK_USER_IMAGES, include/gpt.h) на ВСЕХ
 * обнаруженных дисках и вход в систему по найденному зашифрованному
 * образу (include/userimg.h, Фаза 3) поверх cryptoblk (Фаза 4).
 *
 * ВАЖНО -- см. общий комментарий в начале apps/create-user.c: это
 * понятие "пользователь" (запись в user-images, привязанная к диску,
 * с PBKDF2-паролем и собственным зашифрованным домом) НЕ то же самое,
 * что kernel/users.c (модель прав шелла/ядра, root/user в памяти).
 * Этот файл -- мост между ними: после успешной проверки пароля здесь
 * kernel/users.c заводит (или переиспользует) соответствующую запись
 * в своей таблице сеанса, чтобы whoami/id/su и проверки прав шелла
 * продолжали работать как раньше -- см. users_login_prompt(),
 * kernel/users.c.
 *
 * Чисто kernel-side код (в отличие от apps/create-user.c) -- ядро уже
 * внутри себя умеет читать сырые секторы (storage_read_sectors(),
 * include/storage.h) и монтировать зашифрованный дом
 * (fs_mount_home_encrypted(), include/vfs.h) без syscall-обвязки. */

/* Разумный потолок на число одновременно показанных на экране входа
 * учётных записей с диска -- то же значение, что USERS_MAX
 * (include/users.h), по той же причине (это не жёсткий лимит формата,
 * просто фиксированный размер буфера сканирования). */
#define USERDISK_MAX_ENTRIES 32u

typedef struct {
    char username[USERIMG_USERNAME_LEN + 1]; /* гарантированно NUL-terminated копия поля из заголовка */
    u32  drive_index;                         /* storage.h индекс диска, где найден этот слот */
    u32  slot_header_lba;                     /* абсолютный LBA заголовка слота (userimg_header_t) */
} userdisk_entry_t;

/* Сканирует ВСЕ диски (storage_drive_count()/storage_get_drive()):
 * на каждом ищет раздел user-images через GPT (gpt_parse()), и если
 * находит -- проходит по всем слотам фиксированного размера
 * (USERIMG_SLOT_SECTORS) и заполняет out[] непустыми (magic совпал,
 * см. userimg_header_parse()) записями. Останавливается, когда либо
 * кончились диски, либо заполнен out[] (max_entries).
 *
 * Возвращает число реально найденных записей (0 -- совершенно
 * нормальный результат: диск(и) ещё не размечены под Atomic, или
 * размечены, но ни одного пользователя ещё не создано через
 * apps/create-user.c -- это НЕ ошибка, вызывающий код -- см.
 * users_login_prompt(), kernel/users.c -- в этом случае просто не
 * показывает список и работает как раньше, только через встроенную
 * таблицу root/user). */
u32 userdisk_scan(userdisk_entry_t out[USERDISK_MAX_ENTRIES]);

/* Проверяет пароль для конкретной записи (обязана быть одной из тех,
 * что вернул userdisk_scan() в ЭТОМ же цикле сканирования -- заново
 * читает заголовок слота с диска по entry->drive_index/slot_header_lba,
 * не полагается на устаревшие данные) и, при успехе, сразу монтирует
 * расшифрованный дом через fs_mount_home_encrypted() (readonly=FALSE
 * -- рабочая сессия пользователя должна уметь писать в свой дом).
 *
 * Возвращает FALSE и на "неверный пароль", и на "битый/пропавший со
 * времени сканирования слот", и на "не удалось смонтировать тело" --
 * ОДНА И ТА ЖЕ ошибка на все три случая (см. общий комментарий в
 * include/userimg.h за тем, почему это сознательно, не только для
 * первых двух). При FALSE активный backend VFS НЕ меняется. */
bool_t userdisk_login(const userdisk_entry_t *entry, const char *password, u32 password_len);

#endif

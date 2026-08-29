#ifndef CRYPTOBLK_H
#define CRYPTOBLK_H

#include "types.h"
#include "crypto_xts.h"

/* cryptoblk.h -- ATOMIC_PLAN.md, Фаза 4: тонкий слой, который делает
 * из "сырого" посекторного I/O (что угодно, что читает/пишет секторы
 * по номеру -- см. cryptoblk_read_fn/write_fn ниже) ПРОЗРАЧНО
 * зашифрованное блочное устройство поверх AES-XTS-256 (lib/crypto_xts.c,
 * Фаза 1). Именно этим слоем `fs/ufs.c` (или что угодно ещё,
 * работающее поверх секторов) сможет читать/писать тело
 * пользовательского образа (include/userimg.h, Фаза 3), ни разу не
 * зная о шифровании -- он просто получает и отдаёт ПЛОСКИЙ plaintext
 * поверх cryptoblk_read()/write(), а шифрованием занимается уже
 * read_fn/write_fn под капотом.
 *
 * ПОЧЕМУ dependency injection (функции-указатели), а не жёсткая
 * привязка к sys_disk_read_raw()/write_raw() или storage_read_sectors()/
 * write_sectors() -- у ЭТОГО кода будет минимум ДВА совершенно разных
 * потребителя с разными способами добраться до диска:
 *   - userspace-инструменты (ring 3, через syscalls sys_disk_read_raw()/
 *     write_raw() -- как apps/create-user.c уже делает БЕЗ шифрования
 *     для заголовка) -- тестировался бы через них.
 *   - kernel-side FS-драйверы (ring 0, через storage_read_sectors()/
 *     write_sectors() напрямую, БЕЗ syscall-барьера -- см. fs/ufs.c) --
 *     именно так будет смонтирован UFS поверх расшифрованного тела в
 *     будущей Фазе 5 (вход в систему).
 * Оборачивать разные вызовы в ОДИНАКОВУЮ сигнатуру
 * cryptoblk_read_fn/write_fn -- дешевле и надёжнее, чем дублировать
 * саму крипто-логику (encrypt/decrypt по тюку) под каждого потребителя
 * отдельно, ровно как install-os.c и fs/ufs.c уже независимо
 * дублируют разбиение "count сектор" на чанки под 255 штук -- та же
 * идея, но для крипто-обёртки сделана ОДИН раз, а не дважды.
 *
 * lba здесь -- ЛОКАЛЬНЫЙ номер сектора относительно начала САМОГО
 * зашифрованного устройства (0 = первый сектор ТЕЛА образа, ПОСЛЕ
 * заголовка userimg.h -- см. общий комментарий в apps/create-user.c за
 * тем, где именно начинается тело), а НЕ абсолютный LBA физического
 * диска -- так тюк не меняется, если тело образа когда-нибудь
 * переедет на другой диапазон LBA (переразметка/копирование), и
 * cryptoblk сам не обязан ничего знать про физическое расположение --
 * это ответственность read_fn/write_fn (и того, что их вызывающему
 * коду известно про смещение слота в user-images, ATOMIC_PLAN.md Фаза 3). */

typedef bool_t (*cryptoblk_read_fn)(void *io_ctx, u32 lba, u32 count, void *buf);
typedef bool_t (*cryptoblk_write_fn)(void *io_ctx, u32 lba, u32 count, const void *buf);

typedef struct {
    aes_xts_key_t xk;
    cryptoblk_read_fn read_fn;
    cryptoblk_write_fn write_fn;
    void *io_ctx; /* передаётся read_fn/write_fn как есть -- контекст вызывающего (например, номер диска + смещение слота) */
} cryptoblk_dev_t;

/* Выводит оба XTS-подключа из master_key (aes_xts_key_derive_from_master(),
 * lib/crypto_xts.c) и запоминает read_fn/write_fn/io_ctx. master_key --
 * тот, что вернул userimg_verify_password() (include/userimg.h) после
 * успешного ввода пароля -- этот файл сам паролей не проверяет и не
 * знает про них вообще, только про уже разблокированный ключ. */
void cryptoblk_init(cryptoblk_dev_t *dev, const u8 master_key[XTS_MASTER_KEY_LEN],
                     cryptoblk_read_fn read_fn, cryptoblk_write_fn write_fn, void *io_ctx);

/* Читает/пишет count PLAINTEXT-секторов начиная с локального lba (см.
 * общий комментарий выше). Внутри -- чтение/запись ЦИФРОТЕКСТА через
 * read_fn/write_fn чанками (см. CRYPTOBLK_MAX_BATCH_SECTORS в
 * lib/cryptoblk.c) с шифрованием/расшифровкой каждого сектора
 * отдельно (AES-XTS и так посекторный, батчинг здесь -- только ради
 * меньшего числа вызовов read_fn/write_fn, не меняет сам крипто).
 * Возвращает FALSE при первой же ошибке read_fn/write_fn (частично
 * обработанные секторы буфера caller'а в этом случае не определены). */
bool_t cryptoblk_read(cryptoblk_dev_t *dev, u32 lba, u32 count, void *buf);
bool_t cryptoblk_write(cryptoblk_dev_t *dev, u32 lba, u32 count, const void *buf);

#endif

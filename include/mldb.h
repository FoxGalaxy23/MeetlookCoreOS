#ifndef MLDB_H
#define MLDB_H

#include "types.h"

/* mldb.h -- MLDB ("MeetlookDB"): маленькая однофайловая табличная база
 * данных для пользовательских (ring 3) бинарников MeetlookCoreOS.
 *
 * ЗАЧЕМ. Файловые syscall'ы этой ОС (sys_fs_write_file/sys_fs_read_file,
 * см. include/usyscall.h) работают только с файлом ЦЕЛИКОМ -- нет ни
 * seek/pread/pwrite, ни отображения в память. Поэтому вместо того, чтобы
 * плодить в rootfs кучу мелких конфигурационных файлов (по одному на
 * каждый параметр/список приложения), MLDB держит ВСЁ (таблицы, схему,
 * строки) в одной in-memory структуре и целиком сериализует/
 * десериализует её в/из ОДНОГО файла. С точки зрения VFS это ничем не
 * отличается от записи обычного файла -- вся "база данных" for VFS
 * выглядит как один blob, а не как файловая система внутри файловой
 * системы.
 *
 * МОДЕЛЬ ДАННЫХ. База -- это набор таблиц (до MLDB_MAX_TABLES). Таблица
 * -- фиксированная схема из колонок (до MLDB_MAX_COLUMNS), каждая имеет
 * имя и тип (INT/TEXT/BLOB), плюс произвольное число строк, которые
 * добавляются/читаются/удаляются по индексу. Никаких индексов,
 * транзакций или SQL здесь нет -- это осознанно: цель библиотеки --
 * заменить россыпь плоских конфиг-файлов одним компактным хранилищем
 * "таблица строк с типами", а не быть полноценной СУБД.
 *
 * ВЛАДЕНИЕ ПАМЯТЬЮ. Все mldb_value_t, которые библиотека возвращает
 * вызывающему коду (mldb_get_row/mldb_select), указывают на память,
 * принадлежащую самой базе (действительна, пока жива mldb_t и пока
 * строка не удалена/не изменена) -- копировать их не нужно, но и
 * освобождать (free()) их вызывающий код НЕ должен. Значения, которые
 * вызывающий код передаёт библиотеке (mldb_insert/mldb_update_cell),
 * библиотека, наоборот, копирует себе сама -- буфер, из которого они
 * взяты (например, локальный char[]), можно спокойно переиспользовать
 * или освободить сразу после вызова.
 *
 * ПОТОКОБЕЗОПАСНОСТЬ. mldb_t не потокобезопасна -- у каждого процесса/
 * приложения предполагается своя приватная база (или внешняя блокировка
 * вызывающим кодом), совместного доступа из нескольких задач библиотека
 * не предусматривает. */

#define MLDB_MAX_TABLES    16
#define MLDB_MAX_COLUMNS   16
#define MLDB_NAME_LEN      32   /* включая '\0' */

/* Коды ошибок (все функции, возвращающие s32, при ошибке возвращают
 * один из них; при успехе -- >= 0). */
#define MLDB_ERR_NOTABLE   (-1)  /* нет таблицы/строки/колонки с таким именем/индексом */
#define MLDB_ERR_EXISTS    (-2)  /* таблица с таким именем уже существует */
#define MLDB_ERR_LIMIT     (-3)  /* превышен MLDB_MAX_TABLES/MLDB_MAX_COLUMNS/MLDB_NAME_LEN */
#define MLDB_ERR_SCHEMA    (-4)  /* число/типы значений не совпадают со схемой таблицы, либо TEXT/BLOB длиннее max_len */
#define MLDB_ERR_NOMEM     (-5)  /* malloc/realloc вернул NULL */
#define MLDB_ERR_IO        (-6)  /* sys_fs_write_file/sys_fs_read_file вернул ошибку */
#define MLDB_ERR_FORMAT    (-7)  /* файл не в формате MLDB (magic/version/структура повреждены) */

typedef enum {
    MLDB_INT  = 0,  /* 64-битное знаковое целое */
    MLDB_TEXT = 1,  /* байты + длина; удобный терминатор '\0' НЕ хранится отдельно */
    MLDB_BLOB = 2   /* произвольные байты + длина, семантически то же самое, что TEXT */
} mldb_type_t;

typedef struct {
    char name[MLDB_NAME_LEN];
    mldb_type_t type;
    u32 max_len;   /* верхняя граница длины для TEXT/BLOB (0 = не ограничено); игнорируется для INT */
} mldb_column_t;

typedef struct {
    mldb_type_t type;
    s64 i;              /* используется, если type == MLDB_INT */
    const u8 *bytes;    /* используется, если type == MLDB_TEXT/MLDB_BLOB */
    u32 len;             /* длина bytes[] в байтах (для TEXT -- без учёта '\0') */
} mldb_value_t;

/* ---- конструкторы значений (просто удобные обёртки) ---- */
static inline mldb_value_t mldb_v_int(s64 v) {
    mldb_value_t r; r.type = MLDB_INT; r.i = v; r.bytes = 0; r.len = 0; return r;
}
static inline mldb_value_t mldb_v_text(const char *s) {
    mldb_value_t r; r.type = MLDB_TEXT; r.i = 0; r.bytes = (const u8 *)s;
    u32 n = 0; if (s) { while (s[n]) n++; } r.len = n; return r;
}
static inline mldb_value_t mldb_v_blob(const void *p, u32 len) {
    mldb_value_t r; r.type = MLDB_BLOB; r.i = 0; r.bytes = (const u8 *)p; r.len = len; return r;
}

/* Непрозрачные для вызывающего кода структуры (полное определение --
 * в mldb.c). Указатель на них можно передавать между функциями, но
 * трогать поля напрямую не следует -- используйте mldb_table_* геттеры. */
typedef struct mldb_table mldb_table_t;
typedef struct mldb       mldb_t;

/* ---- жизненный цикл базы ---- */
mldb_t *mldb_new(void);           /* новая пустая база в памяти */
void    mldb_free(mldb_t *db);    /* освобождает всю память (таблицы, строки) */

/* Сериализует базу целиком в один файл через sys_fs_write_file().
 * Возвращает 0 при успехе, <0 при ошибке (нет прав, файл не поместился
 * и т.п. -- см. коды в include/sysabi.h, SYSCALL_ERR_*). */
s32 mldb_save(mldb_t *db, const char *filename);

/* Читает файл целиком через sys_fs_read_file() и разбирает его как базу
 * MLDB. Возвращает новую mldb_t* при успехе (её нужно будет освободить
 * mldb_free()), NULL -- если файла нет, он повреждён/не MLDB-формата,
 * или не хватило памяти. max_file_size -- верхняя граница на размер
 * файла (используется как размер временного буфера чтения). */
mldb_t *mldb_load(const char *filename, u32 max_file_size);

/* ---- схема (таблицы/колонки) ---- */

/* Создаёт таблицу с именем name и заданными колонками. Возвращает 0 при
 * успехе, <0 если: таблица с таким именем уже есть, достигнут
 * MLDB_MAX_TABLES/MLDB_MAX_COLUMNS, имя таблицы/колонки длиннее
 * MLDB_NAME_LEN-1, либо не хватило памяти. */
s32 mldb_create_table(mldb_t *db, const char *name,
                       const mldb_column_t *columns, u32 ncolumns);

/* Удаляет таблицу вместе со всеми её строками. 0 при успехе, <0 если
 * таблицы с таким именем нет. */
s32 mldb_drop_table(mldb_t *db, const char *name);

/* Возвращает указатель на таблицу или NULL, если такой нет. */
mldb_table_t *mldb_find_table(mldb_t *db, const char *name);

u32         mldb_table_count(const mldb_t *db);
const char *mldb_table_name_at(const mldb_t *db, u32 index); /* NULL, если index вне диапазона */

const char       *mldb_table_name(const mldb_table_t *t);
u32                mldb_table_ncolumns(const mldb_table_t *t);
const mldb_column_t *mldb_table_column(const mldb_table_t *t, u32 index); /* NULL, если index вне диапазона */
u32                mldb_table_nrows(const mldb_table_t *t);

/* ---- строки ---- */

/* Добавляет строку в таблицу table. values/nvalues должны в точности
 * соответствовать схеме таблицы (порядок и количество колонок; типы
 * должны совпадать с типами колонок). Данные из values копируются в
 * саму базу -- буфер, из которого они взяты, можно переиспользовать
 * сразу после вызова. Возвращает индекс новой строки (>= 0) или <0 при
 * ошибке (нет такой таблицы, несовпадение схемы, TEXT/BLOB длиннее
 * max_len колонки, либо не хватило памяти). */
s32 mldb_insert(mldb_t *db, const char *table,
                 const mldb_value_t *values, u32 nvalues);

/* Копирует значения строки row_index таблицы table в out_values
 * (максимум max_values штук, обычно == числу колонок таблицы).
 * Указатели bytes в out_values указывают на память самой базы (см.
 * общий комментарий выше про владение памятью) -- копировать их не
 * нужно, но они становятся недействительны при следующем изменении
 * этой строки/таблицы. Возвращает число реально записанных значений
 * (>= 0) или <0, если таблицы/строки с таким индексом нет. */
s32 mldb_get_row(mldb_t *db, const char *table, u32 row_index,
                  mldb_value_t *out_values, u32 max_values);

/* Удаляет строку row_index (последующие строки сдвигаются вниз на одну
 * позицию -- индексы после удалённой строки меняются). 0 при успехе,
 * <0, если таблицы/строки с таким индексом нет. */
s32 mldb_delete_row(mldb_t *db, const char *table, u32 row_index);

/* Заменяет одну ячейку (row_index, col_index) новым значением того же
 * типа, что и колонка. 0 при успехе, <0 при несовпадении типа/индекса
 * вне диапазона/нехватке памяти. */
s32 mldb_update_cell(mldb_t *db, const char *table, u32 row_index,
                      u32 col_index, const mldb_value_t *value);

/* Обратный вызов для mldb_select(): values/nvalues -- строка таблицы (та
 * же семантика владения памятью, что и у mldb_get_row). Возврат FALSE
 * останавливает перебор досрочно (аналог "break"), TRUE -- продолжить
 * со следующей строкой. */
typedef bool_t (*mldb_row_cb)(u32 row_index, const mldb_value_t *values,
                               u32 nvalues, void *ctx);

/* Перебирает все строки таблицы по порядку, вызывая cb на каждой.
 * Возвращает число строк, на которых был вызван cb, либо <0, если
 * таблицы с таким именем нет. */
s32 mldb_select(mldb_t *db, const char *table, mldb_row_cb cb, void *ctx);

#endif /* MLDB_H */

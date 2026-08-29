/* mldb.c -- реализация MLDB (см. include/mldb.h за полным описанием
 * формата и мотивации).
 *
 * Всё хранилище -- обычная куча из lib/libc.c (malloc/realloc/free,
 * приватная для каждого бинарника, см. общий комментарий в libc.h).
 * На диск база попадает ЦЕЛИКОМ одним sys_fs_write_file() -- отсюда
 * двухпроходная сериализация ниже (mldb_writer_t): первый проход
 * считает итоговый размер (buf == NULL), второй пишет в уже выделенный
 * буфер ровно того же размера. Это исключает отдельную "growable
 * buffer" арифметику (realloc в цикле) ценой обхода структуры дважды --
 * учитывая ожидаемый размер конфигурационных баз (килобайты, не
 * мегабайты), это более чем приемлемо. */

#include "mldb.h"
#include "libc.h"
#include "string.h"
#include "usyscall.h"

#define MLDB_MAGIC   0x4442444Cu /* "LDBD" в памяти little-endian == байты 'M','L','D','B' по смещениям 0..3 */
#define MLDB_VERSION 1u

struct mldb_table {
    char          name[MLDB_NAME_LEN];
    mldb_column_t columns[MLDB_MAX_COLUMNS];
    u32           ncolumns;
    mldb_value_t **rows;     /* rows[i] -- malloc'd массив из ncolumns mldb_value_t */
    u32           nrows;
    u32           rows_cap;
};

struct mldb {
    mldb_table_t tables[MLDB_MAX_TABLES];
    u32          ntables;
};

/* ---------------------------------------------------------------------
 * Вспомогательные функции
 * --------------------------------------------------------------------- */

static s32 find_table_index(mldb_t *db, const char *name) {
    for (u32 i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, name) == 0) return (s32)i;
    }
    return MLDB_ERR_NOTABLE;
}

/* Освобождает bytes-буфер одной ячейки, если он был выделен (TEXT/BLOB). */
static void free_cell(mldb_value_t *cell) {
    if (cell->type != MLDB_INT && cell->bytes) {
        free((void *)cell->bytes);
    }
}

static void free_row(mldb_table_t *t, mldb_value_t *row) {
    for (u32 ci = 0; ci < t->ncolumns; ci++) free_cell(&row[ci]);
    free(row);
}

/* Освобождает все строки таблицы (саму запись таблицы -- нет, она living
 * внутри db->tables[] статически). Используется и mldb_drop_table(), и
 * mldb_free(). */
static void free_table_rows(mldb_table_t *t) {
    for (u32 ri = 0; ri < t->nrows; ri++) free_row(t, t->rows[ri]);
    free(t->rows);
    t->rows = 0;
    t->nrows = 0;
    t->rows_cap = 0;
}

/* Копирует одно значение value в новую, независимо владеемую ячейку out
 * (для TEXT/BLOB выделяет и копирует буфер). Проверяет соответствие
 * типу/max_len колонки col. Возвращает 0 при успехе, <0 при ошибке --
 * out в этом случае не трогается. */
static s32 copy_value_into_cell(const mldb_column_t *col, const mldb_value_t *value,
                                 mldb_value_t *out) {
    if (value->type != col->type) return MLDB_ERR_SCHEMA;

    if (col->type == MLDB_INT) {
        out->type = MLDB_INT;
        out->i = value->i;
        out->bytes = 0;
        out->len = 0;
        return 0;
    }

    if (col->max_len != 0 && value->len > col->max_len) return MLDB_ERR_SCHEMA;

    u8 *copy = 0;
    if (value->len > 0) {
        copy = (u8 *)malloc(value->len);
        if (!copy) return MLDB_ERR_NOMEM;
        memcpy(copy, value->bytes, value->len);
    }
    out->type = col->type;
    out->i = 0;
    out->bytes = copy;
    out->len = value->len;
    return 0;
}

/* ---------------------------------------------------------------------
 * Жизненный цикл базы
 * --------------------------------------------------------------------- */

mldb_t *mldb_new(void) {
    mldb_t *db = (mldb_t *)malloc(sizeof(mldb_t));
    if (!db) return 0;
    memset(db, 0, sizeof(*db));
    return db;
}

void mldb_free(mldb_t *db) {
    if (!db) return;
    for (u32 ti = 0; ti < db->ntables; ti++) free_table_rows(&db->tables[ti]);
    free(db);
}

/* ---------------------------------------------------------------------
 * Схема
 * --------------------------------------------------------------------- */

s32 mldb_create_table(mldb_t *db, const char *name,
                       const mldb_column_t *columns, u32 ncolumns) {
    if (!db || !name || !columns) return MLDB_ERR_SCHEMA;
    if (strlen(name) >= MLDB_NAME_LEN) return MLDB_ERR_LIMIT;
    if (ncolumns == 0 || ncolumns > MLDB_MAX_COLUMNS) return MLDB_ERR_LIMIT;
    if (db->ntables >= MLDB_MAX_TABLES) return MLDB_ERR_LIMIT;
    if (find_table_index(db, name) >= 0) return MLDB_ERR_EXISTS;

    for (u32 ci = 0; ci < ncolumns; ci++) {
        if (strlen(columns[ci].name) >= MLDB_NAME_LEN) return MLDB_ERR_LIMIT;
        if (columns[ci].type != MLDB_INT && columns[ci].type != MLDB_TEXT &&
            columns[ci].type != MLDB_BLOB) return MLDB_ERR_SCHEMA;
    }

    mldb_table_t *t = &db->tables[db->ntables];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, MLDB_NAME_LEN - 1);
    t->ncolumns = ncolumns;
    for (u32 ci = 0; ci < ncolumns; ci++) {
        memset(&t->columns[ci], 0, sizeof(mldb_column_t));
        strncpy(t->columns[ci].name, columns[ci].name, MLDB_NAME_LEN - 1);
        t->columns[ci].type = columns[ci].type;
        t->columns[ci].max_len = columns[ci].max_len;
    }
    db->ntables++;
    return 0;
}

s32 mldb_drop_table(mldb_t *db, const char *name) {
    if (!db || !name) return MLDB_ERR_NOTABLE;
    s32 idx = find_table_index(db, name);
    if (idx < 0) return idx;

    free_table_rows(&db->tables[idx]);

    /* Сдвигаем хвост массива таблиц на одну позицию влево, чтобы не
     * оставлять "дырку" -- индексы таблиц (mldb_table_name_at) после
     * удалённой при этом смещаются, что задокументировано в mldb.h
     * тем же образом, что и для строк (mldb_delete_row). */
    for (u32 i = (u32)idx; i + 1 < db->ntables; i++) {
        db->tables[i] = db->tables[i + 1];
    }
    db->ntables--;
    memset(&db->tables[db->ntables], 0, sizeof(mldb_table_t));
    return 0;
}

mldb_table_t *mldb_find_table(mldb_t *db, const char *name) {
    if (!db || !name) return 0;
    s32 idx = find_table_index(db, name);
    return idx >= 0 ? &db->tables[idx] : 0;
}

u32 mldb_table_count(const mldb_t *db) { return db ? db->ntables : 0; }

const char *mldb_table_name_at(const mldb_t *db, u32 index) {
    if (!db || index >= db->ntables) return 0;
    return db->tables[index].name;
}

const char *mldb_table_name(const mldb_table_t *t) { return t ? t->name : 0; }
u32 mldb_table_ncolumns(const mldb_table_t *t) { return t ? t->ncolumns : 0; }

const mldb_column_t *mldb_table_column(const mldb_table_t *t, u32 index) {
    if (!t || index >= t->ncolumns) return 0;
    return &t->columns[index];
}

u32 mldb_table_nrows(const mldb_table_t *t) { return t ? t->nrows : 0; }

/* ---------------------------------------------------------------------
 * Строки
 * --------------------------------------------------------------------- */

s32 mldb_insert(mldb_t *db, const char *table,
                 const mldb_value_t *values, u32 nvalues) {
    mldb_table_t *t = mldb_find_table(db, table);
    if (!t) return MLDB_ERR_NOTABLE;
    if (nvalues != t->ncolumns) return MLDB_ERR_SCHEMA;

    mldb_value_t *row = (mldb_value_t *)malloc(t->ncolumns * sizeof(mldb_value_t));
    if (!row) return MLDB_ERR_NOMEM;

    for (u32 ci = 0; ci < t->ncolumns; ci++) {
        s32 rc = copy_value_into_cell(&t->columns[ci], &values[ci], &row[ci]);
        if (rc < 0) {
            /* откатываем уже скопированные ячейки этой строки */
            for (u32 j = 0; j < ci; j++) free_cell(&row[j]);
            free(row);
            return rc;
        }
    }

    if (t->nrows >= t->rows_cap) {
        u32 newcap = t->rows_cap ? t->rows_cap * 2 : 8;
        mldb_value_t **newrows = (mldb_value_t **)realloc(t->rows, newcap * sizeof(mldb_value_t *));
        if (!newrows) {
            free_row(t, row);
            return MLDB_ERR_NOMEM;
        }
        t->rows = newrows;
        t->rows_cap = newcap;
    }

    t->rows[t->nrows] = row;
    t->nrows++;
    return (s32)(t->nrows - 1);
}

s32 mldb_get_row(mldb_t *db, const char *table, u32 row_index,
                  mldb_value_t *out_values, u32 max_values) {
    mldb_table_t *t = mldb_find_table(db, table);
    if (!t) return MLDB_ERR_NOTABLE;
    if (row_index >= t->nrows) return MLDB_ERR_NOTABLE;

    u32 n = t->ncolumns < max_values ? t->ncolumns : max_values;
    for (u32 ci = 0; ci < n; ci++) out_values[ci] = t->rows[row_index][ci];
    return (s32)n;
}

s32 mldb_delete_row(mldb_t *db, const char *table, u32 row_index) {
    mldb_table_t *t = mldb_find_table(db, table);
    if (!t) return MLDB_ERR_NOTABLE;
    if (row_index >= t->nrows) return MLDB_ERR_NOTABLE;

    free_row(t, t->rows[row_index]);
    for (u32 i = row_index; i + 1 < t->nrows; i++) t->rows[i] = t->rows[i + 1];
    t->nrows--;
    return 0;
}

s32 mldb_update_cell(mldb_t *db, const char *table, u32 row_index,
                      u32 col_index, const mldb_value_t *value) {
    mldb_table_t *t = mldb_find_table(db, table);
    if (!t) return MLDB_ERR_NOTABLE;
    if (row_index >= t->nrows || col_index >= t->ncolumns) return MLDB_ERR_NOTABLE;

    mldb_value_t newcell;
    s32 rc = copy_value_into_cell(&t->columns[col_index], value, &newcell);
    if (rc < 0) return rc;

    free_cell(&t->rows[row_index][col_index]);
    t->rows[row_index][col_index] = newcell;
    return 0;
}

s32 mldb_select(mldb_t *db, const char *table, mldb_row_cb cb, void *ctx) {
    mldb_table_t *t = mldb_find_table(db, table);
    if (!t) return MLDB_ERR_NOTABLE;
    if (!cb) return MLDB_ERR_SCHEMA;

    u32 called = 0;
    for (u32 ri = 0; ri < t->nrows; ri++) {
        called++;
        if (!cb(ri, t->rows[ri], t->ncolumns, ctx)) break;
    }
    return (s32)called;
}

/* ---------------------------------------------------------------------
 * Сериализация -- см. общий комментарий в шапке файла про двухпроходную
 * запись (writer.buf == NULL на первом проходе -- только считаем pos).
 * --------------------------------------------------------------------- */

typedef struct { u8 *buf; u32 pos; } mldb_writer_t;

static void w_bytes(mldb_writer_t *w, const void *data, u32 n) {
    if (w->buf && n) memcpy(w->buf + w->pos, data, n);
    w->pos += n;
}
static void w_u8(mldb_writer_t *w, u8 v)   { w_bytes(w, &v, 1); }
static void w_u32(mldb_writer_t *w, u32 v) { w_bytes(w, &v, 4); }
static void w_s64(mldb_writer_t *w, s64 v) { w_bytes(w, &v, 8); }

static void serialize_body(mldb_t *db, mldb_writer_t *w) {
    w_u32(w, MLDB_MAGIC);
    w_u32(w, MLDB_VERSION);
    w_u32(w, db->ntables);

    for (u32 ti = 0; ti < db->ntables; ti++) {
        mldb_table_t *t = &db->tables[ti];
        u8 nlen = (u8)strlen(t->name);
        w_u8(w, nlen);
        w_bytes(w, t->name, nlen);
        w_u32(w, t->ncolumns);

        for (u32 ci = 0; ci < t->ncolumns; ci++) {
            mldb_column_t *c = &t->columns[ci];
            u8 cnlen = (u8)strlen(c->name);
            w_u8(w, cnlen);
            w_bytes(w, c->name, cnlen);
            w_u8(w, (u8)c->type);
            w_u32(w, c->max_len);
        }

        w_u32(w, t->nrows);
        for (u32 ri = 0; ri < t->nrows; ri++) {
            mldb_value_t *row = t->rows[ri];
            for (u32 ci = 0; ci < t->ncolumns; ci++) {
                if (row[ci].type == MLDB_INT) {
                    w_s64(w, row[ci].i);
                } else {
                    w_u32(w, row[ci].len);
                    w_bytes(w, row[ci].bytes, row[ci].len);
                }
            }
        }
    }
}

s32 mldb_save(mldb_t *db, const char *filename) {
    if (!db || !filename) return MLDB_ERR_SCHEMA;

    mldb_writer_t w = { 0, 0 };
    serialize_body(db, &w);           /* проход 1: только считаем размер */

    u32 size = w.pos;
    u8 *buf = (u8 *)malloc(size ? size : 1);
    if (!buf) return MLDB_ERR_NOMEM;

    w.buf = buf;
    w.pos = 0;
    serialize_body(db, &w);           /* проход 2: пишем в буфер */

    s32 rc = sys_fs_write_file(filename, buf, size);
    free(buf);
    return rc < 0 ? MLDB_ERR_IO : 0;
}

/* ---------------------------------------------------------------------
 * Десериализация
 * --------------------------------------------------------------------- */

typedef struct { const u8 *buf; u32 pos; u32 size; } mldb_reader_t;

static bool_t r_bytes(mldb_reader_t *r, void *out, u32 n) {
    if (r->pos + n > r->size || r->pos + n < r->pos /* переполнение */) return FALSE;
    if (out) memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return TRUE;
}
static bool_t r_u8(mldb_reader_t *r, u8 *v)   { return r_bytes(r, v, 1); }
static bool_t r_u32(mldb_reader_t *r, u32 *v) { return r_bytes(r, v, 4); }
static bool_t r_s64(mldb_reader_t *r, s64 *v) { return r_bytes(r, v, 8); }

/* Читает имя длиной (u8 len + len байт), максимум MLDB_NAME_LEN-1,
 * с завершающим '\0' -- используется и для имени таблицы, и для имени
 * колонки. */
static bool_t r_name(mldb_reader_t *r, char out[MLDB_NAME_LEN]) {
    u8 len = 0;
    if (!r_u8(r, &len) || len >= MLDB_NAME_LEN) return FALSE;
    if (!r_bytes(r, out, len)) return FALSE;
    out[len] = 0;
    return TRUE;
}

mldb_t *mldb_load(const char *filename, u32 max_file_size) {
    if (!filename || max_file_size == 0) return 0;

    u8 *filebuf = (u8 *)malloc(max_file_size);
    if (!filebuf) return 0;

    u32 filesize = 0;
    if (sys_fs_read_file(filename, filebuf, max_file_size, &filesize) < 0) {
        free(filebuf);
        return 0;
    }

    mldb_reader_t r = { filebuf, 0, filesize };
    mldb_t *db = 0;

    u32 magic = 0, version = 0, ntables = 0;
    if (!r_u32(&r, &magic) || magic != MLDB_MAGIC) goto done;
    if (!r_u32(&r, &version) || version != MLDB_VERSION) goto done;
    if (!r_u32(&r, &ntables) || ntables > MLDB_MAX_TABLES) goto done;

    db = mldb_new();
    if (!db) goto done;

    for (u32 ti = 0; ti < ntables; ti++) {
        char tname[MLDB_NAME_LEN];
        if (!r_name(&r, tname)) goto fail;

        u32 ncolumns = 0;
        if (!r_u32(&r, &ncolumns) || ncolumns == 0 || ncolumns > MLDB_MAX_COLUMNS) goto fail;

        mldb_column_t cols[MLDB_MAX_COLUMNS];
        for (u32 ci = 0; ci < ncolumns; ci++) {
            char cname[MLDB_NAME_LEN];
            u8 type = 0;
            u32 max_len = 0;
            if (!r_name(&r, cname)) goto fail;
            if (!r_u8(&r, &type) || type > MLDB_BLOB) goto fail;
            if (!r_u32(&r, &max_len)) goto fail;
            strncpy(cols[ci].name, cname, MLDB_NAME_LEN - 1);
            cols[ci].name[MLDB_NAME_LEN - 1] = 0;
            cols[ci].type = (mldb_type_t)type;
            cols[ci].max_len = max_len;
        }

        if (mldb_create_table(db, tname, cols, ncolumns) < 0) goto fail;

        u32 nrows = 0;
        if (!r_u32(&r, &nrows)) goto fail;

        for (u32 ri = 0; ri < nrows; ri++) {
            mldb_value_t values[MLDB_MAX_COLUMNS];
            for (u32 ci = 0; ci < ncolumns; ci++) {
                if (cols[ci].type == MLDB_INT) {
                    s64 iv = 0;
                    if (!r_s64(&r, &iv)) goto fail;
                    values[ci] = mldb_v_int(iv);
                } else {
                    u32 len = 0;
                    if (!r_u32(&r, &len)) goto fail;
                    if (r.pos + len > r.size) goto fail;
                    values[ci].type = cols[ci].type;
                    values[ci].i = 0;
                    values[ci].bytes = r.buf + r.pos; /* mldb_insert копирует немедленно */
                    values[ci].len = len;
                    r.pos += len;
                }
            }
            if (mldb_insert(db, tname, values, ncolumns) < 0) goto fail;
        }
    }

    goto done;

fail:
    mldb_free(db);
    db = 0;

done:
    free(filebuf);
    return db;
}

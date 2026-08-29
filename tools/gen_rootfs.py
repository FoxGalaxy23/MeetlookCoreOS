#!/usr/bin/env python3
"""
gen_rootfs.py -- упаковывает содержимое каталога rootfs/ в C-массивы,
которые RAMFS распаковывает в памяти во время ramfs_init().

Использование:
    python3 tools/gen_rootfs.py

Читает:  rootfs/**            (всё, что лежит в этой папке, попадёт в RAMFS)
Пишет:   fs/ramfs_data.c
         include/ramfs_data.h

Соглашение об приложениях:
    Любой файл в rootfs/bin/<name> без точки в имени помечается как
    RAMFS_APP_BINARY -- он считается исполняемым пользовательским
    бинарником и может быть запущен загрузчиком ядра (kernel/loader.c)
    командой "run <name>" или просто вводом имени в shell. Такие файлы
    собираются из apps/*.c целью `apps` в Makefile (см. apps/user.ld) --
    "make rootfs" сама вызывает "make apps" перед упаковкой. Остальные
    файлы -- обычные (RAMFS_APP_NONE).

Файл в rootfs/ соответствует пути в RAMFS без префикса "rootfs":
    rootfs/readme.txt   -> /readme.txt
    rootfs/bin/calc     -> /bin/calc
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTFS_DIR = os.path.join(ROOT, "rootfs")
OUT_C = os.path.join(ROOT, "fs", "ramfs_data.c")
OUT_H = os.path.join(ROOT, "include", "ramfs_data.h")

def collect_files(rootfs_dir):
    entries = []
    for dirpath, _dirnames, filenames in os.walk(rootfs_dir):
        filenames.sort()
        for fname in sorted(filenames):
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, rootfs_dir)
            vfs_path = "/" + rel.replace(os.sep, "/")
            entries.append((vfs_path, full))
    entries.sort(key=lambda e: e[0])
    return entries


def app_id_for(vfs_path):
    parts = vfs_path.split("/")
    if len(parts) == 3 and parts[1] == "bin" and "." not in parts[2]:
        return "RAMFS_APP_BINARY"
    return "RAMFS_APP_NONE"


def c_array_name(index):
    return "ramfs_seed_data_%d" % index


def emit_byte_array(out, name, data):
    out.write("static const u8 %s[] = {\n" % name)
    line = []
    for i, b in enumerate(data):
        line.append("0x%02x," % b)
        if len(line) == 16:
            out.write("    " + " ".join(line) + "\n")
            line = []
    if line:
        out.write("    " + " ".join(line) + "\n")
    if not data:
        out.write("    0\n")  # избегаем массива нулевой длины
    out.write("};\n\n")


def main():
    if not os.path.isdir(ROOTFS_DIR):
        print("rootfs/ directory not found at %s" % ROOTFS_DIR, file=sys.stderr)
        return 1

    entries = collect_files(ROOTFS_DIR)

    with open(OUT_C, "w", encoding="utf-8") as out:
        out.write("/* Автогенерируемый файл -- НЕ РЕДАКТИРОВАТЬ РУКАМИ.\n")
        out.write("   Собран из каталога rootfs/ скриптом tools/gen_rootfs.py.\n")
        out.write("   Чтобы изменить базовые файлы RAMFS, редактируйте rootfs/\n")
        out.write("   и запустите `make rootfs` (или `python3 tools/gen_rootfs.py`). */\n\n")
        out.write('#include "ramfs_data.h"\n\n')

        for i, (_vfs_path, full) in enumerate(entries):
            with open(full, "rb") as f:
                data = f.read()
            emit_byte_array(out, c_array_name(i), data)

        out.write("const ramfs_seed_file_t ramfs_seed_files[] = {\n")
        for i, (vfs_path, full) in enumerate(entries):
            size = os.path.getsize(full)
            app_id = app_id_for(vfs_path)
            out.write(
                '    { "%s", %s, %d, %s },\n'
                % (vfs_path, c_array_name(i), size, app_id)
            )
        out.write("};\n\n")
        out.write(
            "const u32 ramfs_seed_file_count = %d;\n" % len(entries)
        )

    with open(OUT_H, "w", encoding="utf-8") as out:
        out.write("/* Автогенерируемый файл -- НЕ РЕДАКТИРОВАТЬ РУКАМИ.\n")
        out.write("   Собран из каталога rootfs/ скриптом tools/gen_rootfs.py. */\n\n")
        out.write("#ifndef RAMFS_DATA_H\n#define RAMFS_DATA_H\n\n")
        out.write('#include "types.h"\n')
        out.write('#include "ramfs.h"\n\n')
        out.write("typedef struct {\n")
        out.write("    const char *path;   /* полный путь в RAMFS, напр. \"/bin/calc\" */\n")
        out.write("    const u8 *data;\n")
        out.write("    u32 size;\n")
        out.write("    ramfs_app_id_t app_id;\n")
        out.write("} ramfs_seed_file_t;\n\n")
        out.write("extern const ramfs_seed_file_t ramfs_seed_files[];\n")
        out.write("extern const u32 ramfs_seed_file_count;\n\n")
        out.write("#endif\n")

    print("Generated %s and %s from %d file(s) in rootfs/" % (OUT_C, OUT_H, len(entries)))
    for vfs_path, full in entries:
        print("  %s  (%d bytes, app=%s)" % (vfs_path, os.path.getsize(full), app_id_for(vfs_path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

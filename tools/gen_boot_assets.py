#!/usr/bin/env python3
"""
gen_boot_assets.py -- генерирует kernel/boot_assets_data.c: три массива
байт (stage1.bin, stage2.bin, kernel.flat), встроенные прямо в .rodata
ядра. См. include/boot_assets.h за тем, почему НЕ через RAMFS (лимит
RAMFS_FILE_DATA_SIZE=65536 байт/файл, include/ramfs.h -- kernel.flat
много больше).

Использование:
    python3 tools/gen_boot_assets.py <stage1.bin> <stage2.bin> <kernel.flat> \
        <stage1_gpt.bin> <stage2_atomic.bin>

Пишет: kernel/boot_assets_data.c

ВАЖНО -- см. INSTALLER_PLAN.md, раздел "Самоссылающийся kernel.flat":
kernel.flat, переданный сюда, встраивается в СЛЕДУЮЩУЮ сборку ядра --
т.е. это всегда должен быть kernel.flat ИЗ ПРЕДЫДУЩЕГО прохода сборки,
не совпадающий byte-in-byte с результатом текущей (это математически
невозможно для самоссылающегося образа, см. подробности в плане).
tools/build_installer_assets.sh уже вызывает этот скрипт в правильном
месте двухпроходной сборки -- обычно не нужно запускать вручную.

stage1_gpt.bin/stage2_atomic.bin -- ATOMIC_PLAN.md, Фаза 6 (см. общий
комментарий у BOOT_ASSET_STAGE1_ATOMIC, include/boot_assets.h) --
собираются из ТЕХ ЖЕ .s исходников, что stage1.bin/stage2.bin (первый
-- буквально boot/stage1.s с другим -D, второй -- отдельный
boot/stage2_atomic.s), но НЕ участвуют в двухпроходной
самоссылающейся сборке -- в отличие от kernel.flat, они не встроены
сами в себя ни с какой стороны, поэтому передаются сюда как обычные,
независимые от прохода файлы.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "kernel", "boot_assets_data.c")

ASSETS = [
    ("g_boot_asset_stage1", "stage1.bin"),
    ("g_boot_asset_stage2", "stage2.bin"),
    ("g_boot_asset_kernel_flat", "kernel.flat"),
    ("g_boot_asset_stage1_atomic", "stage1_gpt.bin"),
    ("g_boot_asset_stage2_atomic", "stage2_atomic.bin"),
]


def emit_byte_array(out, name: str, data: bytes) -> None:
    out.write(f"static const u8 {name}[] = {{\n")
    if data:
        line = []
        for i, b in enumerate(data):
            line.append("0x%02x," % b)
            if len(line) == 16:
                out.write("    " + " ".join(line) + "\n")
                line = []
        if line:
            out.write("    " + " ".join(line) + "\n")
    else:
        out.write("    0\n")  # избегаем массива нулевой длины
    out.write("};\n\n")


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)

    paths = sys.argv[1:6]
    blobs = []
    for path in paths:
        with open(path, "rb") as f:
            blobs.append(f.read())

    with open(OUT_C, "w") as out:
        out.write("/* Автогенерируемый файл -- НЕ РЕДАКТИРОВАТЬ РУКАМИ.\n")
        out.write("   Собран скриптом tools/gen_boot_assets.py из:\n")
        for path in paths:
            out.write(f"     {os.path.relpath(path, ROOT)}\n")
        out.write("   См. include/boot_assets.h и INSTALLER_PLAN.md,\n")
        out.write('   раздел "Самоссылающийся kernel.flat". */\n\n')
        out.write('#include "boot_assets.h"\n\n')

        for (name, _label), data in zip(ASSETS, blobs):
            emit_byte_array(out, name, data)

        out.write("static const boot_asset_entry_t g_boot_assets[BOOT_ASSET_COUNT] = {\n")
        for (name, _label), data in zip(ASSETS, blobs):
            out.write(f"    {{ {name}, {len(data)}u }},\n")
        out.write("};\n\n")

        out.write("const boot_asset_entry_t *boot_assets_table(void) {\n")
        out.write("    return g_boot_assets;\n")
        out.write("}\n")

    sizes = ", ".join(f"{label}={len(data)}" for (_, label), data in zip(ASSETS, blobs))
    print(f"gen_boot_assets: записано {os.path.relpath(OUT_C, ROOT)} ({sizes})")


if __name__ == "__main__":
    main()

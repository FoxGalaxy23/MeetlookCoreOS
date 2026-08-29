#!/bin/sh
# build_installer_assets.sh -- собирает stage1.bin/stage2.bin/kernel.flat
# (плюс stage1_gpt.bin/stage2_atomic.bin -- ATOMIC_PLAN.md, Фаза 6, см.
# общий комментарий в boot/stage2_atomic.s) и встраивает их прямо в
# .rodata ядра (kernel/boot_assets_data.c, tools/gen_boot_assets.py),
# НЕ через RAMFS -- у RAMFS жёсткий лимит
# RAMFS_FILE_DATA_SIZE=65536 байт/файл (include/ramfs.h), kernel.flat
# много больше (обнаружено вживую: install-os молча писал усечённые до
# 64 KiB байты ядра на диск при первой попытке -- см. общий комментарий
# в include/boot_assets.h). install-os читает эти ассеты через
# SYS_BOOT_ASSET_READ (include/usyscall.h), а не через VFS/sys_fs_read_file.
#
# ДВУХПРОХОДНОСТЬ (см. INSTALLER_PLAN.md, раздел "Самоссылающийся
# kernel.flat"): kernel.flat не может содержать байт-в-байт копию самого
# себя (математически невозможно -- целое не может одновременно быть
# строго больше и равно своей части), поэтому встроенный в
# kernel/boot_assets_data.c asset -- это ВСЕГДА "предыдущее поколение"
# сборки, на один шаг младше того kernel.flat, который реально
# загружается прямо сейчас. Это ожидаемо и безопасно.
#
# Использование:
#     sh tools/build_installer_assets.sh

set -e
cd "$(dirname "$0")/.."

echo "== Проход 0: приложения (нужны и для Прохода 1, и для Прохода 2) =="
make apps
python3 tools/gen_rootfs.py

echo "== Проход 1: kernel.flat БЕЗ самого себя в kernel/boot_assets_data.c =="
if [ ! -f kernel/boot_assets_data.c ]; then
    echo "  (boot_assets_data.c отсутствует -- создаю пустую заглушку для бутстрапа)"
    cat > kernel/boot_assets_data.c << 'EOF'
/* Временная заглушка -- будет перезаписана tools/gen_boot_assets.py
   (см. tools/build_installer_assets.sh). */
#include "boot_assets.h"
static const u8 g_empty[] = { 0 };
static const boot_asset_entry_t g_boot_assets[BOOT_ASSET_COUNT] = {
    { g_empty, 0u }, { g_empty, 0u }, { g_empty, 0u }, { g_empty, 0u }, { g_empty, 0u },
};
const boot_asset_entry_t *boot_assets_table(void) { return g_boot_assets; }
EOF
fi
rm -rf build
make build/stage1.bin build/stage2.bin build/kernel.flat build/stage1_gpt.bin build/stage2_atomic.bin

echo "== Встраиваю ассеты Прохода 1 в kernel/boot_assets_data.c =="
python3 tools/gen_boot_assets.py build/stage1.bin build/stage2.bin build/kernel.flat build/stage1_gpt.bin build/stage2_atomic.bin

echo "== Проход 2: пересобираю ядро -- теперь оно несёт Проход-1 ассеты =="
rm -rf build
make build/kernel.bin build/kernel.flat build/stage1.bin build/stage2.bin build/stage1_gpt.bin build/stage2_atomic.bin

echo "== Тестовый диск-образ (для отладки без install-os, см. NOGRUB_PLAN.md) =="
make build/test_boot.img

echo ""
echo "Готово."
ls -la kernel/boot_assets_data.c build/kernel.flat build/stage1.bin build/stage2.bin
echo ""
echo "Дальше: make build/meetlookcoreos.iso (или используйте build/test_boot.img"
echo "напрямую) -- ЭТО и есть установочный носитель, с которого нужно"
echo "запускать install-os (runc /containers/install-os.manifest -- под root,"
echo "'run install-os' не подойдёт, у него нет caps=disk_raw)."

#!/usr/bin/env python3
"""
make_test_boot_img.py -- собирает тестовый "сырой" диск-образ для
проверки Фазы B (NOGRUB_PLAN.md): stage1 (MBR) + stage2 + kernel.flat,
уложенные по фиксированным LBA, без единого байта GRUB и без ISO.

Это ВРЕМЕННАЯ замена install-os (см. INSTALLER_PLAN.md, Фаза 4, ещё не
реализована) -- то же самое расположение байт, которое install-os в
будущем будет писать через sys_disk_write_raw(), здесь собирается
напрямую в файл-образ обычным Python (никакой настоящий диск не
затрагивается, только файл на хосте/в песочнице).

Использование:
    python3 tools/make_test_boot_img.py <out.img> <stage1.bin> <stage2.bin> <kernel.flat>

Разметка (см. общий комментарий в NOGRUB_PLAN.md):
    LBA 0        -- stage1.bin, ровно 512 байт (MBR)
    LBA 1..63    -- stage2.bin, дополнен нулями до 63 * 512 байт
    LBA 64..32831 -- kernel.flat, дополнен нулями (или обрезан с явной
                    ошибкой, если не влезает) до 32768 * 512 байт (16 MiB --
                    поднято с 2, 4, затем 8 MiB, kernel.flat перерос старые
                    бюджеты после ATOMIC_PLAN.md Фаз 1-7, см. ту же константу в
                    boot/stage2.s и apps/install-os.c)
Дальше (LBA 32832+) в этом тестовом образе ничего нет -- FAT32-раздел
появится вместе с install-os.
"""

import sys

SECTOR_SIZE = 512
STAGE2_LBA_START = 1
STAGE2_SECTORS = 63
KERNEL_LBA_START = 64
KERNEL_SECTORS = 32768  # 16 MiB


def read_exact_or_pad(path: str, size: int) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > size:
        raise SystemExit(
            f"make_test_boot_img: {path} -- {len(data)} байт не влезает "
            f"в отведённые {size} байт"
        )
    return data + b"\x00" * (size - len(data))


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)

    out_path, stage1_path, stage2_path, kernel_path = sys.argv[1:5]

    stage1 = read_exact_or_pad(stage1_path, SECTOR_SIZE)
    if stage1[-2:] != b"\x55\xAA":
        raise SystemExit(
            "make_test_boot_img: stage1.bin не заканчивается сигнатурой "
            "0x55 0xAA -- проверьте boot/stage1.s (times ... db 0 / dw 0xAA55)"
        )

    stage2 = read_exact_or_pad(stage2_path, STAGE2_SECTORS * SECTOR_SIZE)
    kernel = read_exact_or_pad(kernel_path, KERNEL_SECTORS * SECTOR_SIZE)

    total_size = (KERNEL_LBA_START + KERNEL_SECTORS) * SECTOR_SIZE
    image = bytearray(total_size)
    image[0 : len(stage1)] = stage1
    image[STAGE2_LBA_START * SECTOR_SIZE : STAGE2_LBA_START * SECTOR_SIZE + len(stage2)] = stage2
    image[KERNEL_LBA_START * SECTOR_SIZE : KERNEL_LBA_START * SECTOR_SIZE + len(kernel)] = kernel

    with open(out_path, "wb") as f:
        f.write(image)

    print(
        f"make_test_boot_img: записано {out_path} "
        f"({len(image)} байт = {len(image) // SECTOR_SIZE} секторов)"
    )


if __name__ == "__main__":
    main()

OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .text :
    {
        boot.o(.text)
        *(.text)
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(.bss)
    }

    /* Паддинг нулями до 510-го байта от начала сектора, затем сигнатура */
    . = 0x7C00 + 510;
    .signature :
    {
        SHORT(0xAA55)
    }

    /* Линкер выдаст ошибку, если код не влезает в 510 байт */
    ASSERT(. <= 0x7C00 + 512, "ERROR: Bootloader exceeds 512 bytes!")
}
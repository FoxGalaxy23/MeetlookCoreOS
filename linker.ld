OUTPUT_FORMAT("binary")
ENTRY(_kstart)

SECTIONS
{
    /* Ядро загружается в физический адрес 0x1000 */
    . = 0x1000;

    .text :
    {
        /* ОБЯЗАТЕЛЬНО первым: точка входа из загрузчика */
        kernel_entry.o(.text.entry)
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        *(.bss*)
        *(COMMON)
    }
}
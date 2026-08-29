ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4K) : { *(.multiboot) }
    .text   ALIGN(4K) :
    {
        /* .text.entry -- см. общий комментарий про _start_native в
           boot/boot.s. Обязана идти ПЕРВОЙ внутри выходной секции .text,
           чтобы _start_native оказался по предсказуемому, фиксированному
           адресу (1M + 4K, т.к. .text уже выровнена на 4K после
           12-байтного .multiboot) -- это единственный способ для
           boot/stage2.s (у которого нет доступа к таблице символов ELF,
           только фиксированные оффсеты в сыром kernel.flat) узнать, куда
           прыгать, не читая ELF-заголовки самостоятельно. */
        *(.text.entry)
        *(.text)
    }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data   ALIGN(4K) : { *(.data) }
    .bss    ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }

    kernel_end = .;
}

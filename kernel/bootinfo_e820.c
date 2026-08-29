/* bootinfo_e820.c -- переводит сырую карту памяти BIOS INT 15h/EAX=0xE820
 * (см. boot/stage2.s, get_e820), собранную ЕЩЁ ДО перехода в Long Mode,
 * в архитектурно-нейтральный boot_info_t (include/bootinfo.h,
 * NOGRUB_PLAN.md Фаза B). Зеркало kernel/bootinfo_multiboot.c -- тот
 * переходник для GRUB-пути, этот -- для собственного stage1/stage2.
 *
 * Вызывается из boot/boot.s (_start_native/long_mode_start_native) ДО
 * kernel_main() -- ровно как и bootinfo_from_multiboot(), поэтому та же
 * оговорка про статическое хранение (см. общий комментарий в
 * kernel/bootinfo_multiboot.c): динамическая аллокация здесь ещё
 * недоступна.
 *
 * ФОРМАТ ВХОДНЫХ ДАННЫХ. e820_buf -- физический адрес (идентично
 * отображённый, см. boot/boot.s: paging уже включён к моменту вызова)
 * буфера с entry_count подряд идущими 20-байтными записями формата BIOS
 * E820 (см. get_e820 в boot/stage2.s):
 *   offset  0: u64 base
 *   offset  8: u64 length
 *   offset 16: u32 type   (1 = usable; 2/3/4/5 = резерв в разных смыслах --
 *                           см. ACPI спецификацию; мы не различаем их
 *                           тоньше, чем "usable" и "всё остальное", ровно
 *                           как и BOOTINFO_MEM_RESERVED сейчас используется
 *                           только с этим единственным значением по смыслу)
 * Это НЕ C-структура с выравниванием компилятора -- сырой, побайтовый
 * формат BIOS, куда его and записал 16-битный реальный режим (см. общий
 * комментарий в include/bootinfo.h про то, почему подобный разбор
 * сознательно не делается прямо в асме stage2.s).
 *
 * ТРЕТИЙ ПАРАМЕТР (atomic_info_ptr) -- ATOMIC_PLAN.md, Фаза 6: 0, если
 * грузимся НЕ через boot/stage2_atomic.s (обычный NOGRUB stage1/stage2.s
 * -- см. там xor ecx, ecx перед прыжком -- либо GRUB/Multiboot, тот
 * путь вообще не проходит через эту функцию), иначе -- физический адрес
 * блока формата ATOMIC_BOOTINFO_MAGIC (см. общий комментарий у него,
 * include/bootinfo.h, за точной раскладкой). */

#include "bootinfo.h"

#define E820_ENTRY_SIZE 20u
#define E820_TYPE_USABLE 1u

static boot_info_t g_boot_info;

static u64 read_u64_le(const u8 *p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; i++) v |= ((u64)p[i]) << (8u * i);
    return v;
}

static u32 read_u32_le(const u8 *p) {
    u32 v = 0;
    for (u32 i = 0; i < 4; i++) v |= ((u32)p[i]) << (8u * i);
    return v;
}

const boot_info_t *bootinfo_from_e820(u32 e820_buf, u32 entry_count, u32 atomic_info_ptr) {
    g_boot_info.region_count = 0;

    const u8 *p = (const u8 *)(uintptr_t)e820_buf;
    for (u32 i = 0; i < entry_count; i++) {
        const u8 *entry = p + (u64)i * E820_ENTRY_SIZE;
        u64 base   = read_u64_le(entry + 0);
        u64 length = read_u64_le(entry + 8);
        u32 type   = read_u32_le(entry + 16);

        u32 bi_type = (type == E820_TYPE_USABLE) ? BOOTINFO_MEM_USABLE : BOOTINFO_MEM_RESERVED;
        bootinfo_add_region(&g_boot_info, base, length, bi_type);
    }
    /* Если entry_count == 0 (BIOS не поддержал E820, get_e820 не
     * записал ни одной записи) -- region_count остаётся 0,
     * bootinfo_usable_ram_extent() вернёт 0, и pmm_init() (kernel/pmm.c)
     * сам применит тот же запасной вариант (32 MiB), что и на GRUB-пути
     * без флага MULTIBOOT_INFO_MEMORY -- поведение обоих переходников
     * в этом крайнем случае специально согласовано. */

    /* ---- ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у
     * ATOMIC_BOOTINFO_MAGIC, include/bootinfo.h, за точным форматом.
     * atomic_info_ptr == 0 -- ЗАКОННЫЙ, ожидаемый случай (см. общий
     * комментарий в boot/stage2.s: старый, не-Atomic путь загрузки
     * явно обнуляет этот регистр перед прыжком сюда) -- НЕ пытаемся
     * читать по нулевому адресу. Magic не совпал -- тоже не ошибка в
     * смысле "нужно паниковать": просто мусор/несовместимая версия
     * stage2_atomic.s, и мы ведём себя так, будто это обычная
     * (не-Atomic) загрузка -- ровно та же философия, что у
     * gpt_parse()/bootmeta_parse() (include/gpt.h/bootmeta.h):
     * "не совпало -- значит не оно", не авария. */
    g_boot_info.atomic_boot_valid = FALSE;
    g_boot_info.atomic_boot_drive = 0;
    g_boot_info.atomic_bootmeta_lba = 0;
    g_boot_info.atomic_active_slot = 0;
    if (atomic_info_ptr != 0) {
        const u8 *a = (const u8 *)(uintptr_t)atomic_info_ptr;
        u32 magic = read_u32_le(a + 0);
        if (magic == ATOMIC_BOOTINFO_MAGIC) {
            g_boot_info.atomic_boot_valid = TRUE;
            g_boot_info.atomic_boot_drive = (u32)a[4];
            g_boot_info.atomic_active_slot = a[5];
            /* a[6..7] -- reserved, сознательно не читаем */
            g_boot_info.atomic_bootmeta_lba = read_u32_le(a + 8);
        }
    }

    return &g_boot_info;
}

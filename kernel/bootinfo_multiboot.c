/* bootinfo_multiboot.c -- переводит то, что передал GRUB (magic +
 * multiboot_info_t), в архитектурно-нейтральный boot_info_t (см.
 * include/bootinfo.h, NOGRUB_PLAN.md Фаза A). ЕДИНСТВЕННЫЙ файл во всём
 * дереве, которому вообще позволено включать multiboot.h -- весь код
 * ниже kernel_main() (pmm.c и далее) про Multiboot больше ничего не
 * знает и знать не должен.
 *
 * Вызывается один-единственный раз, из boot/boot.s, ДО kernel_main() --
 * то есть до того, как готова куча ядра (kheap_init() ещё не вызван) и
 * до того, как вообще инициализирован PMM (pmm_init() как раз получает
 * результат этой функции на вход). Поэтому boot_info_t не может быть
 * выделен динамически -- это единственный статический объект в файле,
 * ровно тот же приём, каким kernel/pmm.c хранит свою битовую карту
 * (g_bitmap) в .bss, а не в куче. Повторный вызов перезаписал бы тот же
 * объект -- но повторных вызовов в текущей архитектуре загрузки и не
 * бывает (одна точка входа, один загрузчик за сессию). */

#include "bootinfo.h"
#include "multiboot.h"

static boot_info_t g_boot_info;

const boot_info_t *bootinfo_from_multiboot(u32 magic, u32 mb_info_addr) {
    /* Magic (0x2BADB002 у настоящего Multiboot1-загрузчика) исторически
     * не проверялся и в прежнем коде (kernel_main() просто делал
     * (void)magic;) -- сохраняем то же поведение один в один, чтобы
     * Фаза A была чисто структурным рефакторингом без изменения
     * поведения на GRUB-пути. Параметр оставлен в сигнатуре ради
     * симметрии с тем, что реально приходит от загрузчика в регистрах,
     * и на случай, если проверку захотят добавить позже. */
    (void)magic;

    g_boot_info.region_count = 0;

    const multiboot_info_t *mbi = (const multiboot_info_t *)(uintptr_t)mb_info_addr;
    if (mbi != 0 && (mbi->flags & MULTIBOOT_INFO_MEMORY)) {
        /* mem_upper -- КиБ памяти начиная с 1 МиБ (см. общий комментарий
         * в include/multiboot.h). GRUB не сообщает mem_lower отдельным
         * usable-регионом -- ровно как и в прежнем pmm.c, mem_lower
         * (обычная память 0..640 КиБ) в расчёт не берётся: это тот же
         * диапазон, который pmm_init() и так безусловно резервирует
         * целиком под BIOS/реальный режим/таблицы страниц (см.
         * pmm_reserve_region((void *)0x0, 0x100000u) в kernel/pmm.c). */
        u64 total_bytes = 0x100000ull + (u64)mbi->mem_upper * 1024ull;
        bootinfo_add_region(&g_boot_info, 0, total_bytes, BOOTINFO_MEM_USABLE);
    }
    /* Если GRUB не предоставил информацию о памяти -- region_count
     * остаётся 0, bootinfo_usable_ram_extent() вернёт 0, и pmm_init()
     * (kernel/pmm.c) сам применит запасной вариант (32 MiB) -- ровно то
     * же поведение, что было раньше при отсутствующем флаге
     * MULTIBOOT_INFO_MEMORY. */

    return &g_boot_info;
}

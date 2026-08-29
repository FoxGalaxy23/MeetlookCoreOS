#ifndef BOOTINFO_H
#define BOOTINFO_H

#include "types.h"

/* bootinfo.h -- см. NOGRUB_PLAN.md, Фаза A. Архитектурно-нейтральный
 * "конверт" информации о загрузке, который kernel_main() и всё, что
 * дальше по цепочке (сейчас -- только kernel/pmm.c), получают вместо
 * прямой работы с multiboot_info_t.
 *
 * ИДЕЯ. Раньше пришлось бы разбирать multiboot_info_t. Каждый новый
 * bootloader (свой x86 stage2, будущие ARM/RISC-V-порты через Device
 * Tree) заводил бы свой собственный формат передачи этой же по смыслу
 * информации -- и pmm.c пришлось бы либо дублировать под каждый из них,
 * либо обвешивать #ifdef'ами. Вместо этого -- ОДНА общая структура
 * (эта), которую заполняет отдельный, специфичный для конкретного
 * загрузчика переходник (см. kernel/bootinfo_multiboot.c -- пока
 * единственный; в будущем появится kernel/bootinfo_fdt.c для
 * Device Tree). Сам kernel_main() и всё, что после него, про
 * Multiboot/FDT/что угодно ещё ничего не знают -- работают только с
 * boot_info_t.
 *
 * Единственное, что сюда сейчас положено, -- карта физической памяти:
 * ровно то, что pmm_init() (kernel/pmm.c) реально использует. Новые
 * поля (framebuffer, cmdline, initrd) добавляются по мере надобности --
 * старый код, который их не читает, от такого расширения не страдает
 * (тот же принцип "растущего ABI", что уже применён у вас в sysabi.h). */

#define BOOTINFO_MEM_USABLE   1u /* можно отдавать под pmm_alloc_frame() */
#define BOOTINFO_MEM_RESERVED 2u /* существует физически, но занято (BIOS/ACPI/MMIO и т.п.) --
                                     пока нигде не используется (Multiboot MEMORY-тег не различает
                                     резервы отдельно от "дыр"), но тип уже заведён на будущее:
                                     полная карта e820 (см. NOGRUB_PLAN.md, Фаза B, stage2) будет
                                     содержать именно такие регионы. */

/* ATOMIC_PLAN.md, Фаза 6 -- сырой (побайтовый, НЕ C-структура с
 * выравниванием компилятора -- та же оговорка, что и у формата E820 в
 * kernel/bootinfo_e820.c) формат блока, который boot/stage2_atomic.s
 * кладёт в фиксированное место в нижней памяти и передаёт указатель на
 * него третьим регистром в _start_native (см. общий комментарий в
 * boot/boot.s) -- ТОЛЬКО когда грузимся с GPT/Atomic-диска через
 * boot-meta. bootinfo_from_e820() (kernel/bootinfo_e820.c) разбирает
 * его в поля ниже (atomic_boot_*):
 *   offset 0  : u32 magic (LE) -- ATOMIC_BOOTINFO_MAGIC, отличить
 *               настоящий блок от мусора (регистр не обнулён должным
 *               образом и т.п.), тот же принцип, что magic у bootmeta.h
 *   offset 4  : u8  drive_index (см. atomic_boot_drive ниже)
 *   offset 5  : u8  active_slot (BOOTMETA_SLOT_A/B, include/bootmeta.h)
 *   offset 6  : u8[2] reserved (всегда 0)
 *   offset 8  : u32 bootmeta_lba (LE, абсолютный LBA сектора boot-meta)
 *   offset 12 : конец полезной нагрузки (ATOMIC_BOOTINFO_SIZE) */
#define ATOMIC_BOOTINFO_MAGIC 0x4D4C4142u /* "MLAB" -- MeetLook Atomic Boot, тот же
                                              принцип отображения символов, что у
                                              BOOTMETA_MAGIC ("MLBT"), include/bootmeta.h */
#define ATOMIC_BOOTINFO_SIZE  12u

typedef struct {
    u64 base;
    u64 length;
    u32 type; /* BOOTINFO_MEM_* */
} boot_mem_region_t;

#define BOOTINFO_MAX_MEM_REGIONS 32

typedef struct {
    u32 region_count;
    boot_mem_region_t regions[BOOTINFO_MAX_MEM_REGIONS];

    /* BIOS-номер загрузочного диска (0x80+N для N-го жёсткого диска,
       см. общий комментарий у SYS_GET_BOOT_DRIVE, include/sysabi.h, и
       INSTALLER_PLAN.md, раздел "Защита от самоуничтожения") --
       используется install-os, чтобы предупредить, если выбранный для
       установки диск похож на диск, с которого сейчас загружена сама
       система. bios_boot_drive_valid=FALSE, если загрузчик не сообщил
       эту информацию (например, GRUB без флага MULTIBOOT_INFO_BOOTDEV,
       или загрузка не с BIOS-совместимого устройства вообще). */
    u32 bios_boot_drive;
    bool_t bios_boot_drive_valid;

    /* ---- ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий в
     * kernel/bootinfo_e820.c (bootinfo_from_e820(), третий параметр) и
     * boot/stage2_atomic.c за тем, как это заполняется. Заполнено ТОЛЬКО
     * если загрузка прошла через boot/stage2_atomic.s (GPT/Atomic-диск,
     * с boot-meta) -- atomic_boot_valid=FALSE во ВСЕХ остальных случаях
     * (GRUB/Multiboot-путь, старый NOGRUB stage1/stage2.s без
     * boot-meta) -- ровно тот же принцип, что и у bios_boot_drive_valid
     * чуть выше: отсутствие информации -- нормальный, ожидаемый случай,
     * не ошибка. */
    bool_t atomic_boot_valid;
    u32  atomic_boot_drive;      /* индекс диска в терминах storage.h (drivers/storage.c) --
                                     см. общий комментарий в boot/stage2_atomic.s за тем, почему
                                     это ВСЕГДА 0 в текущей реализации (единственное упрощение
                                     Фазы 6, см. ATOMIC_PLAN.md) */
    u32  atomic_bootmeta_lba;    /* абсолютный LBA сектора boot-meta на atomic_boot_drive --
                                     нужен kernel/bootconfirm.c, чтобы знать, КУДА записать
                                     boot_confirmed=1 после успешного входа в систему */
    u8   atomic_active_slot;     /* BOOTMETA_SLOT_A/B (include/bootmeta.h) -- слот, ИЗ КОТОРОГО
                                     реально загрузились (после автооткатов stage2_atomic.s,
                                     если они были) -- НЕ обязательно совпадает с тем, что было
                                     записано в boot-meta ДО перезагрузки */
} boot_info_t;

/* Добавляет регион в bi->regions[], если есть место. Молча ничего не
 * делает, если region_count уже достиг BOOTINFO_MAX_MEM_REGIONS --
 * потерять лишний регион на игрушечной ОС не катастрофа (32 региона --
 * щедрый запас даже для полной карты e820 с типичной материнской
 * платы), в отличие от, например, потери байта данных пользователя, для
 * такого случая падать/паниковать было бы неоправданно. */
void bootinfo_add_region(boot_info_t *bi, u64 base, u64 length, u32 type);

/* Возвращает "верхнюю границу пригодной для использования физической
 * памяти" -- максимум (base+length) среди ВСЕХ регионов типа
 * BOOTINFO_MEM_USABLE (не сумма длин -- в карте могут быть дыры, нам
 * нужна именно верхняя граница, ровно как раньше pmm_init() считал
 * total_bytes из mem_upper). Возвращает 0, если usable-регионов нет
 * вовсе (bi->region_count == 0 либо все регионы BOOTINFO_MEM_RESERVED)
 * -- вызывающий код (pmm_init(), kernel/pmm.c) сам решает запасной
 * вариант для этого случая, как делал и раньше для "GRUB не передал
 * информацию о памяти". */
u64 bootinfo_usable_ram_extent(const boot_info_t *bi);

#endif

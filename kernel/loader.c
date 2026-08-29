/* loader.c -- запуск пользовательских бинарников.
 *
 * См. include/loader.h за описанием обоих режимов исполнения
 * (loader_run_file_sandboxed -- ring 3 + syscalls, по умолчанию;
 * loader_run_file_god -- старое ring 0 + kapi_t, только root через
 * 'rungod') и include/kapi.h за таблицей функций, которую получает
 * god-режим.
 *
 * Формат бинарника -- настоящий статический ELF64 (ET_EXEC, без
 * PT_INTERP; см. elf_load() ниже за точными проверками) со стандартными
 * PT_LOAD-сегментами вместо старого плоского .text/.rodata/.data-дампа.
 * В sandboxed-режиме (as != NULL) каждый процесс получает СОБСТВЕННОЕ
 * адресное пространство (address_space_t, kernel/paging.c) -- сами
 * PT_LOAD-сегменты по-прежнему обязаны укладываться в фиксированное
 * окно [PROC_CODE_BASE, PROC_CODE_BASE+PROC_CODE_SIZE), см.
 * include/memlayout.h, но так как это окно виртуальное и приватное для
 * каждого процесса (разные CR3 => разные физические кадры под теми же
 * адресами), это НЕ мешает запускать несколько разных бинарников
 * одновременно -- см. kernel/process.c: process_run_sandboxed()/
 * process_fork()/process_execve(). В god-режиме (as == NULL) сегменты
 * по-прежнему копируются прямо по физическим адресам, как раньше. */

#include "loader.h"
#include "kapi.h"
#include "vfs.h"
#include "string.h"
#include "kprintf.h"
#include "console.h"
#include "vga.h"
#include "keyboard.h"
#include "io.h"
#include "storage.h"
#include "rtc.h"
#include "power.h"
#include "pmm.h"
#include "kheap.h"
#include "process.h"
#include "memlayout.h"

/* Символ из boot/linker.ld -- адрес сразу после образа ядра. Используем
   только его адрес (&kernel_end), значение не важно. */
extern u8 kernel_end;

#define MAX_TICK_SERVICES 8

static void (*tick_services[MAX_TICK_SERVICES])(void);
static u32 tick_service_count = 0;

static bool_t loader_register_tick(void (*fn)(void)) {
    if (fn == 0 || tick_service_count >= MAX_TICK_SERVICES) return FALSE;
    tick_services[tick_service_count++] = fn;
    return TRUE;
}

void loader_run_tick_services(void) {
    for (u32 i = 0; i < tick_service_count; i++) {
        tick_services[i]();
    }
}

static const kapi_t g_kapi = {
    .abi_version = MLKAPI_VERSION,

    .kprintf = kprintf,
    .console_readline = console_readline,

    .vga_init = vga_init,
    .vga_clear = vga_clear,
    .vga_putc = vga_putc,
    .vga_puts = vga_puts,
    .vga_set_color = vga_set_color,
    .vga_backspace = vga_backspace,
    .vga_update_cursor = vga_update_cursor,
    .vga_scroll_if_needed = vga_scroll_if_needed,

    .keyboard_getchar = keyboard_getchar,
    .keyboard_haskey = keyboard_haskey,

    .outb = outb,
    .inb = inb,
    .outw = outw,
    .inw = inw,
    .insw = insw,
    .outsw = outsw,
    .io_wait = io_wait,

    .fs_ls = fs_ls,
    .fs_cd = fs_cd,
    .fs_mkdir = fs_mkdir,
    .fs_rm = fs_rm,
    .fs_write_file = fs_write_file,
    .fs_cat = fs_cat,
    .fs_pwd = fs_pwd,
    .fs_read_file = fs_read_file,
    .fs_get_app = fs_get_app,
    .fs_backend_is_fat32 = fs_backend_is_fat32,
    .fs_backend_is_ufs = fs_backend_is_ufs,
    .fs_current_drive = fs_current_drive,

    .storage_drive_count = storage_drive_count,
    .storage_get_drive = storage_get_drive,
    .storage_read_sectors = storage_read_sectors,
    .storage_write_sectors = storage_write_sectors,

    .rtc_read = rtc_read,
    .power_shutdown = power_shutdown,
    .power_reboot = power_reboot,

    .kmalloc = kmalloc,
    .kcalloc = kcalloc,
    .krealloc = krealloc,
    .kfree = kfree,

    .mem_total_bytes = pmm_total_memory_bytes,
    .mem_free_bytes = pmm_free_memory_bytes,
    .heap_total_bytes = kheap_total_bytes,
    .heap_used_bytes = kheap_used_bytes,

    /* run_file в самой kapi_t (т.е. если ring0/god-приложение хочет само
       запустить другой бинарник) сознательно ведёт в god-режим, а не в
       песочницу: у god-режима и так уже есть полный доступ к системе,
       так что понижать себя в ring 3 для дочернего процесса было бы не
       столько "безопасностью", сколько путаницей. Sandboxed-приложения
       (ring 3) kapi_t не получают вообще (см. kernel/usermode.s -- им
       передаётся NULL), так что у них такой возможности нет в принципе. */
    .run_file = loader_run_file_god,
    .register_tick = loader_register_tick,
};

#include "elf.h"

u64 elf_load(const void *elf_data, u32 elf_size, address_space_t *as) {
    if (elf_size < sizeof(Elf64_Ehdr)) {
        kprintf("elf_load: file too small for EHDR (%u bytes)\n", elf_size);
        return 0;
    }
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("elf_load: bad ELF magic\n");
        return 0;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("elf_load: not a 64-bit ELF\n");
        return 0;
    }

    if (ehdr->e_machine != 62) { /* EM_X86_64 = 62 */
        kprintf("elf_load: not an x86_64 ELF\n");
        return 0;
    }

    /* Загружаем только настоящие статические исполняемые файлы: ET_EXEC
       -- фиксированные (не PIE) адреса, никакого relocation-прохода нам
       делать не нужно. ET_DYN (PIE) сюда попал бы с p_vaddr, начинающимися
       около 0 -- такие сегменты либо провалили бы проверку окна ниже с
       малопонятной ошибкой, либо, что хуже, случайно легли бы в
       допустимый диапазон и молча собрались бы не в то место, куда
       рассчитывал компоновщик (базовое смещение мы не вычисляем).
       Отклоняем явно и заранее, с понятным сообщением. */
    if (ehdr->e_type != ET_EXEC) {
        kprintf("elf_load: not a static executable (e_type=%u; PIE/ET_DYN and other "
                "types are not supported -- link with -no-pie -static)\n", ehdr->e_type);
        return 0;
    }

    if (ehdr->e_phoff + (u64)ehdr->e_phnum * ehdr->e_phentsize > elf_size) {
        kprintf("elf_load: program headers out of bounds\n");
        return 0;
    }

    /* Первый проход: только проверки, без единой записи в as. PT_INTERP
       (динамический линковщик, которого у нас нет) и вылезающие за
       пределы процессного окна PT_LOAD-сегменты отклоняются ДО того, как
       мы успели отобразить хоть одну страницу. */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)((const u8 *)elf_data + ehdr->e_phoff + (u64)i * ehdr->e_phentsize);

        if (phdr->p_type == PT_INTERP) {
            kprintf("elf_load: PT_INTERP present -- dynamically linked binaries "
                    "are not supported, link statically (-static)\n");
            return 0;
        }

        if (phdr->p_type == PT_LOAD && as) {
            /* Песочница (as != NULL, ring 3): код/данные процесса обязаны
               уместиться в его собственное окно кода [PROC_CODE_BASE,
               PROC_CODE_BASE + PROC_CODE_SIZE) (include/memlayout.h) --
               без этой проверки специально собранный (или просто битый)
               ELF с p_vaddr, указывающим внутрь окна стека/кучи/shm (те же
               process-window PD-слоты, см. kernel/paging.c:
               slot_is_process_window()), молча отобразился бы поверх них
               через as_alloc_and_map(), портя раскладку прежде, чем стек
               вообще будет построен. God-режим (as == NULL) эту проверку
               сознательно не делает -- там доверенный код и другая модель
               (см. loader_run_file_god). */
            u64 seg_start = phdr->p_vaddr;
            u64 seg_end   = seg_start + phdr->p_memsz;
            if (seg_end < seg_start ||
                seg_start < PROC_CODE_BASE ||
                seg_end   > PROC_CODE_BASE + PROC_CODE_SIZE) {
                kprintf("elf_load: PT_LOAD segment [0x%llx, 0x%llx) outside process "
                        "code window [0x%llx, 0x%llx)\n",
                        seg_start, seg_end,
                        (u64)PROC_CODE_BASE, (u64)PROC_CODE_BASE + PROC_CODE_SIZE);
                return 0;
            }

            /* W^X (см. include/paging.h): доверять голым PF_W/PF_X из
               заголовка сегмента и мапить страницу writable+executable
               одновременно, потому что ELF так попросил, означало бы
               позволить любому специально собранному (или просто
               испорченному) бинарнику одной строчкой в program header
               свести на нет всю защиту, которую даёт W^X в
               kernel/paging.c/kernel/loader.c ниже. Отклоняем такой
               сегмент целиком, ДО того как отобразили хоть одну его
               страницу -- ровно как и проверку границ выше. Обычным,
               честно собранным программам (-static, стандартный
               линковщик) это никогда не мешает: .text отдельно от
               .data/.bss -- разные PT_LOAD-сегменты, ни один не просит
               оба бита сразу. */
            if ((phdr->p_flags & PF_W) && (phdr->p_flags & PF_X)) {
                kprintf("elf_load: PT_LOAD segment [0x%llx, 0x%llx) requests both "
                        "writable and executable (W^X violation) -- rejected\n",
                        seg_start, seg_end);
                return 0;
            }
        }
    }

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)((const u8 *)elf_data + ehdr->e_phoff + (u64)i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            if (phdr->p_offset + phdr->p_filesz > elf_size) {
                kprintf("elf_load: segment offset out of bounds\n");
                return 0;
            }

            if (as) {
                u64 seg_vaddr  = phdr->p_vaddr;
                u64 seg_filesz = phdr->p_filesz;
                u64 seg_memsz  = phdr->p_memsz;
                u64 seg_offset = phdr->p_offset;

                /* Страницы, покрываемые сегментом в виртуальном пространстве. */
                u64 start_page = seg_vaddr & ~4095ull;
                u64 end_page   = (seg_vaddr + seg_memsz + 4095ull) & ~4095ull;

                for (u64 page = start_page; page < end_page; page += 4096ull) {
                    bool_t writable   = (phdr->p_flags & PF_W) != 0;
                    bool_t executable = (phdr->p_flags & PF_X) != 0;
                    void *frame = as_alloc_and_map(as, page, writable, executable);
                    if (!frame) {
                        kprintf("elf_load: failed to map frame at 0x%llx\n", page);
                        return 0;
                    }

                    /* Пересечение виртуального диапазона страницы [page, page+4096)
                       с файловой частью сегмента [seg_vaddr, seg_vaddr+seg_filesz). */
                    u64 copy_vstart = (seg_vaddr > page) ? seg_vaddr : page;
                    u64 page_end_v  = page + 4096ull;
                    u64 seg_fend    = seg_vaddr + seg_filesz;
                    u64 copy_vend   = (seg_fend < page_end_v) ? seg_fend : page_end_v;

                    if (copy_vstart < copy_vend) {
                        /* Смещение внутри физического кадра. */
                        u64 frame_off = copy_vstart - page;
                        /* Смещение внутри файла ELF. */
                        u64 file_off  = seg_offset + (copy_vstart - seg_vaddr);
                        u64 copy_len  = copy_vend - copy_vstart;
                        memcpy((u8 *)frame + frame_off,
                               (const u8 *)elf_data + file_off,
                               copy_len);
                    }
                    /* Остаток страницы (BSS) уже обнулён в as_alloc_and_map(). */
                }
            } else {
                if ((u8 *)phdr->p_vaddr < &kernel_end) {
                    kprintf("elf_load: god mode segment overlaps kernel image\n");
                    return 0;
                }

                if (phdr->p_filesz > 0) {
                    memcpy((void *)phdr->p_vaddr, (const u8 *)elf_data + phdr->p_offset, phdr->p_filesz);
                }
                if (phdr->p_memsz > phdr->p_filesz) {
                    memset((void *)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
                }
            }
        }
    }

    return ehdr->e_entry;
}

s32 loader_run_file_god(const char *name, u32 argc, char **argv) {
    if (fs_get_app(name) != RAMFS_APP_BINARY) {
        kprintf("Not an executable binary: %s\n", name);
        return -1;
    }

    void *buf = kmalloc(RAMFS_FILE_DATA_SIZE);
    if (!buf) {
        kprintf("loader: out of memory for read buffer\n");
        return -1;
    }

    u32 size = 0;
    if (!fs_read_file(name, buf, RAMFS_FILE_DATA_SIZE, &size) || size == 0) {
        kprintf("loader: failed to read binary: %s\n", name);
        kfree(buf);
        return -1;
    }

    u64 entry_point = elf_load(buf, size, NULL);
    kfree(buf);

    if (entry_point == 0) {
        kprintf("loader: failed to load ELF: %s\n", name);
        return -1;
    }

    app_main_t entry = (app_main_t)entry_point;
    return entry(&g_kapi, argc, argv);
}

s32 loader_run_file_sandboxed(const char *name, u32 argc, char **argv) {
    if (fs_get_app(name) != RAMFS_APP_BINARY) {
        kprintf("Not an executable binary: %s\n", name);
        return -1;
    }

    void *buf = kmalloc(RAMFS_FILE_DATA_SIZE);
    if (!buf) {
        kprintf("loader: out of memory for read buffer\n");
        return -1;
    }

    u32 size = 0;
    if (!fs_read_file(name, buf, RAMFS_FILE_DATA_SIZE, &size) || size == 0) {
        kprintf("loader: failed to read binary: %s\n", name);
        kfree(buf);
        return -1;
    }

    s32 ret = process_run_sandboxed(buf, size, argc, argv);
    kfree(buf);
    return ret;
}

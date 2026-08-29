#ifndef BOOTCONFIRM_H
#define BOOTCONFIRM_H

#include "types.h"
#include "bootinfo.h"

/* bootconfirm.h -- ATOMIC_PLAN.md, Фаза 6. Мост между сведениями,
 * которые boot/stage2_atomic.s передал через boot_info_t
 * (atomic_boot_valid/atomic_boot_drive/atomic_bootmeta_lba, см. общий
 * комментарий у ATOMIC_BOOTINFO_MAGIC, include/bootinfo.h), и моментом,
 * когда ядро решает, что дошло до рабочего состояния и пора выставить
 * boot_confirmed=1 в boot-meta на диске (см. SYS_BOOT_CONFIRM,
 * include/sysabi.h). */

/* Вызывается РОВНО один раз, максимально рано в kernel_main() (kernel/kernel.c) --
 * ДО users_init()/users_login_prompt() -- запоминает нужные поля bi
 * во внутреннем static-состоянии этого модуля (сам bi -- временный
 * указатель на буфер в нижней памяти, живущий только пока сохранён этот
 * же boot_info_t, см. общий комментарий в kernel/bootinfo_e820.c за
 * g_boot_info -- полагаться на него после этой точки не стоит, отсюда и
 * копия сюда). */
void bootconfirm_init(const boot_info_t *bi);

/* ATOMIC_PLAN.md, Фаза 6 -- вызывается kernel_main() ОДИН раз, сразу
 * после успешного users_login_prompt() ("дошли до рабочего состояния" --
 * см. общий комментарий у SYS_BOOT_CONFIRM, include/sysabi.h). Если
 * bootconfirm_init() не видела atomic_boot_valid=TRUE (обычная,
 * не-Atomic загрузка -- подавляющее большинство случаев на сегодня,
 * пока нет Фазы 7) -- НИЧЕГО не делает, тихо возвращает FALSE, это НЕ
 * ошибка. Иначе -- перечитывает сектор boot-meta с
 * atomic_boot_drive/atomic_bootmeta_lba, проверяет (magic+CRC32,
 * bootmeta_parse()), что active_slot в нём совпадает с тем, что
 * boot/stage2_atomic.s сообщил при загрузке (защита от TOCTOU --
 * boot-meta теоретически мог измениться между стартом stage2_atomic.s и
 * этим моментом только если что-то ещё пишет на диск в обход этого
 * загруженного экземпляра ядра, что при однопользовательской,
 * однопоточной по отношению к диску модели этого ядра не должно
 * случаться, но проверка дешева и не помешает), и если совпадает --
 * bootmeta_mark_confirmed() + запись обратно. Возвращает TRUE при
 * успехе (включая "уже было подтверждено -- писать было нечего"),
 * FALSE на любую из перечисленных причин отказа (не Atomic-загрузка,
 * несовпадение active_slot, ошибка чтения/записи диска, битый
 * boot-meta) -- ОДНА ошибка на все причины сразу, тот же принцип, что и
 * везде в этом дереве при разборе форматов на диске (gpt_parse()/
 * bootmeta_parse()/userimg_header_parse()) -- вызывающему коду
 * (kernel_main()) достаточно знать "получилось" или "нет", а не
 * различать точную причину, которая ему всё равно ничем не поможет на
 * этом этапе загрузки. */
bool_t bootconfirm_mark_ok(void);

#endif

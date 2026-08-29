#ifndef USYSCALL_H
#define USYSCALL_H

#include "types.h"
#include "sysabi.h"

/* usyscall.h -- обёртки над `int 0x80` для sandboxed (ring 3)
 * пользовательских бинарников. См. include/sysabi.h за номерами и
 * соглашением о регистрах, kernel/syscall.c за диспетчером в ядре.
 *
 * В отличие от kapi_t (include/kapi.h) -- таблицы указателей НА
 * ФУНКЦИИ ЯДРА, которую ring0-приложения (запущенные через 'rungod')
 * получают первым аргументом app_main() -- это обычные функции,
 * слинкованные прямо в тело бинарника (см. Makefile, правило
 * build/apps/%.elf, и apps/user.ld). Вызвать функцию ядра напрямую по
 * указателю из ring 3 всё равно бы не получилось: страницы кода ядра
 * не отмечены как user-accessible (см. kernel/paging.c), так что
 * единственный путь наружу из песочницы -- явный `int 0x80`. */

s32  sys_write(const char *buf, u32 len);
s32  sys_readline(char *buf, u32 maxlen);
s32  sys_getchar(void);
void sys_yield(void);
s32  sys_meminfo(sys_meminfo_t *out);
s32  sys_fork(void);
s32  sys_execve(const char *name, u32 argc, char **argv);
void sys_exit(s32 code) __attribute__((noreturn));

/* waitpid(): pid < 0 -- подождать любого прямого потомка sys_fork();
   pid >= 0 -- подождать конкретно его. status_out может быть NULL.
   Возвращает id завершившегося потомка либо <0, если такого потомка
   нет (в т.ч. если pid никогда не был нашим ребёнком). См.
   include/sysabi.h (SYS_WAITPID) за точной семантикой. */
s32  sys_waitpid(s32 pid, s32 *status_out);

/* ---- IPC -- см. include/sysabi.h (SYS_PIPE_..., SYS_MQ_..., SYS_SHM_...)
   за точным соглашением о хендлах/id и include/ipc.h за общим описанием
   модели. */
s32 sys_pipe_create(s32 fds_out[2]);
s32 sys_pipe_read(s32 h, void *buf, u32 len);
s32 sys_pipe_write(s32 h, const void *buf, u32 len);
s32 sys_pipe_close(s32 h);

s32 sys_mq_create(const char *name_or_null);
s32 sys_mq_send(s32 h, const void *buf, u32 len);
s32 sys_mq_recv(s32 h, void *buf, u32 maxlen);
s32 sys_mq_close(s32 h);

s32 sys_shm_create(const char *name_or_null, u32 size);
s32 sys_shm_attach(s32 id, void **out_vaddr);
s32 sys_shm_detach(void *vaddr);
s32 sys_shm_destroy(s32 id);

/* ---- сеть -- см. include/sysabi.h (SYS_NET_..., SYS_TCP_...) за точным
   соглашением и include/net.h/include/tcp.h за устройством стека в ядре. */
s32 sys_net_status(sys_net_status_t *out);
s32 sys_net_dhcp(u32 timeout_ms);
s32 sys_net_resolve(const char *host, u32 *out_ip);
s32 sys_net_ping(u32 ip, u32 timeout_ms, u32 *out_rtt_ms);
s32 sys_tcp_connect(u32 ip, u16 port);
s32 sys_tcp_send(s32 h, const void *buf, u32 len);
s32 sys_tcp_recv(s32 h, void *buf, u32 maxlen, u32 timeout_ms);
s32 sys_tcp_close(s32 h);

/* ---- файловая система (см. SYS_FS_WRITE_FILE/SYS_FS_READ_FILE/... в
   include/sysabi.h) ---- */
s32 sys_fs_write_file(const char *name, const void *data, u32 len);
s32 sys_fs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size);
s32 sys_fs_stat(const char *name, sys_fs_stat_t *out);
s32 sys_fs_pwd(char *buf, u32 bufsize);
s32 sys_fs_cd(const char *name);
s32 sys_fs_mkdir(const char *name);
s32 sys_fs_rm(const char *name);
s32 sys_fs_list(sys_fs_dirent_t *out, u32 max_entries, u32 *out_count);

/* ---- список задач, сон (см. SYS_PS_LIST/SYS_SLEEP в include/sysabi.h) ---- */
s32 sys_ps_list(sys_task_info_t *out, u32 max_entries, u32 *out_count);
s32 sys_sleep(u32 ms);

/* ---- питание (см. SYS_POWER_REBOOT/SYS_POWER_SHUTDOWN в include/sysabi.h) ---- */
s32 sys_power_reboot(void);
s32 sys_power_shutdown(void);

/* ---- пользователи (см. SYS_USERS_* в include/sysabi.h) ---- */
s32 sys_users_whoami(sys_user_info_t *out);
s32 sys_users_list(sys_user_info_t *out, u32 max_entries, u32 *out_count);
s32 sys_users_authenticate(const char *name, const char *password);
s32 sys_users_read_password(char *buf, u32 maxlen);
s32 sys_users_passwd(const char *target, const char *current_pass_or_null, const char *new_pass);
s32 sys_users_add(const char *name, const char *password, bool_t is_root);
s32 sys_users_del(const char *name);
s32 sys_users_su(const char *target, const char *password_or_null);
s32 sys_users_logout(void);

/* ---- диски/USB/время (см. SYS_STORAGE_LIST/SYS_USB_LIST/
   SYS_USB_MOUSE_STATE/SYS_RTC_READ в include/sysabi.h) ---- */
s32 sys_storage_list(sys_storage_info_t *out, u32 max_entries, u32 *out_count);
s32 sys_usb_list(sys_usb_info_t *out, u32 max_entries, u32 *out_count);
s32 sys_usb_mouse_state(sys_mouse_state_t *out);
s32 sys_rtc_read(sys_rtc_time_t *out);

/* ---- монтирование/форматирование/права, настройка сети (см.
   SYS_FS_MOUNT/SYS_FS_FORMAT/SYS_FS_CHMOD/SYS_FS_CHOWN/
   SYS_FS_BACKEND_INFO/SYS_NET_SET_STATIC в include/sysabi.h) ---- */
s32 sys_fs_mount(u32 drive_index);
s32 sys_fs_format(u32 drive_index, u32 fs_type);
s32 sys_fs_chmod(const char *name, u32 mode);
s32 sys_fs_chown(const char *name, u32 new_uid);
s32 sys_fs_backend_info(sys_fs_backend_info_t *out);
s32 sys_net_set_static(u32 ip, u32 netmask, u32 gateway, u32 dns);

/* ---- Сырой блочный доступ к диску, в обход VFS (см. общий комментарий
   у SYS_DISK_READ_RAW/SYS_DISK_WRITE_RAW, include/sysabi.h). Требует
   CAP_DISK_RAW (include/container.h) -- без неё вернёт
   SYSCALL_ERR_PERM. count -- число 512-байтных секторов, buf должен
   вмещать как минимум count*512 байт. */
s32 sys_disk_read_raw(u32 drive_index, u32 lba, u32 count, void *buf);
s32 sys_disk_write_raw(u32 drive_index, u32 lba, u32 count, const void *buf);
s32 sys_fs_format_home_encrypted(u32 drive_index, u32 slot_body_lba_start,
                                  u32 slot_body_sectors, const u8 *master_key);

/* Boot-ассеты для install-os (см. include/boot_assets.h) -- asset_id:
   0=stage1, 1=stage2, 2=kernel.flat, 3=stage1_atomic, 4=stage2_atomic.
   Требует CAP_DISK_RAW. */
s32 sys_boot_asset_read(u32 asset_id, void *buf, u32 bufsize, u32 *out_size);

/* ATOMIC_PLAN.md, Фаза 7 -- см. общий комментарий у
   SYS_BOOT_ASSET_WRITE_TO_DISK/VERIFY_ON_DISK, include/sysabi.h --
   пишут/сверяют ассет напрямую между .rodata ядра и диском, без
   userspace-буфера размером с весь ассет (kernel.flat давно перерос
   APP_MAX_SIZE). Требуют CAP_DISK_RAW. */
s32 sys_boot_asset_write_to_disk(u32 asset_id, u32 drive_index, u32 lba_start, u32 region_sectors);
s32 sys_boot_asset_verify_on_disk(u32 asset_id, u32 drive_index, u32 lba_start);

/* ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у SYS_BOOT_CONFIRM,
   include/sysabi.h. Требует CAP_DISK_RAW + root. */
s32 sys_boot_confirm(void);

/* ---- контейнеры (см. SYS_CONTAINER_SPAWN в include/sysabi.h,
   include/container.h за форматом манифеста) ---- */
s32 sys_container_spawn(const char *manifest_path, u32 argc, char **argv);

/* ---- IRQ (см. SYS_IRQ_WAIT в include/sysabi.h, include/irqbridge.h) ---- */
s32 sys_irq_wait(u32 irq);

/* ---- мост кадров ядро<->драйвер-контейнер (см. SYS_NETBRIDGE_* в
   include/sysabi.h, include/netbridge.h за полным описанием) ---- */
s32 sys_netbridge_tx_dequeue(void *buf, u32 maxlen);
s32 sys_netbridge_rx_enqueue(const void *buf, u32 len);

/* ---- Фаза 5, подшаг 5.5 (см. SYS_CLEAR/SYS_FS_USE_RAMFS в include/sysabi.h) ---- */
s32 sys_clear(void);
s32 sys_fs_use_ramfs(void);

#endif

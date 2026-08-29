/* usyscall.c -- см. include/usyscall.h. Линкуется в каждый
 * пользовательский бинарник (см. Makefile), как и lib/string.c. */

#include "usyscall.h"

static inline s64 syscall4(u64 num, u64 a1, u64 a2, u64 a3, u64 a4) {
    s64 ret;
    /* `int 0x80` (в отличие от инструкции `syscall`) не трогает rcx/r11
       особым образом, поэтому 4-й аргумент можно спокойно передавать в
       rcx, как обычный System V AMD64 аргумент. */
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "D" (a1), "S" (a2), "d" (a3), "c" (a4)
        : "memory"
    );
    return ret;
}

s32 sys_write(const char *buf, u32 len) {
    return (s32)syscall4(SYS_WRITE, (u64)(uintptr_t)buf, len, 0, 0);
}

s32 sys_readline(char *buf, u32 maxlen) {
    return (s32)syscall4(SYS_READLINE, (u64)(uintptr_t)buf, maxlen, 0, 0);
}

s32 sys_getchar(void) {
    return (s32)syscall4(SYS_GETCHAR, 0, 0, 0, 0);
}

void sys_yield(void) {
    syscall4(SYS_YIELD, 0, 0, 0, 0);
}

s32 sys_meminfo(sys_meminfo_t *out) {
    return (s32)syscall4(SYS_MEMINFO, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_fork(void) {
    return (s32)syscall4(SYS_FORK, 0, 0, 0, 0);
}

s32 sys_execve(const char *name, u32 argc, char **argv) {
    return (s32)syscall4(SYS_EXECVE, (u64)(uintptr_t)name, argc, (u64)(uintptr_t)argv, 0);
}

s32 sys_waitpid(s32 pid, s32 *status_out) {
    return (s32)syscall4(SYS_WAITPID, (u64)(s64)pid, (u64)(uintptr_t)status_out, 0, 0);
}

void sys_exit(s32 code) {
    syscall4(SYS_EXIT, (u64)(s64)code, 0, 0, 0);
    for (;;) { } /* не достигается: SYS_EXIT никогда не возвращается */
}

s32 sys_pipe_create(s32 fds_out[2]) {
    return (s32)syscall4(SYS_PIPE_CREATE, (u64)(uintptr_t)fds_out, 0, 0, 0);
}

s32 sys_pipe_read(s32 h, void *buf, u32 len) {
    return (s32)syscall4(SYS_PIPE_READ, (u64)(s64)h, (u64)(uintptr_t)buf, len, 0);
}

s32 sys_pipe_write(s32 h, const void *buf, u32 len) {
    return (s32)syscall4(SYS_PIPE_WRITE, (u64)(s64)h, (u64)(uintptr_t)buf, len, 0);
}

s32 sys_pipe_close(s32 h) {
    return (s32)syscall4(SYS_PIPE_CLOSE, (u64)(s64)h, 0, 0, 0);
}

s32 sys_mq_create(const char *name_or_null) {
    return (s32)syscall4(SYS_MQ_CREATE, (u64)(uintptr_t)name_or_null, 0, 0, 0);
}

s32 sys_mq_send(s32 h, const void *buf, u32 len) {
    return (s32)syscall4(SYS_MQ_SEND, (u64)(s64)h, (u64)(uintptr_t)buf, len, 0);
}

s32 sys_mq_recv(s32 h, void *buf, u32 maxlen) {
    return (s32)syscall4(SYS_MQ_RECV, (u64)(s64)h, (u64)(uintptr_t)buf, maxlen, 0);
}

s32 sys_mq_close(s32 h) {
    return (s32)syscall4(SYS_MQ_CLOSE, (u64)(s64)h, 0, 0, 0);
}

s32 sys_shm_create(const char *name_or_null, u32 size) {
    return (s32)syscall4(SYS_SHM_CREATE, (u64)(uintptr_t)name_or_null, size, 0, 0);
}

s32 sys_shm_attach(s32 id, void **out_vaddr) {
    return (s32)syscall4(SYS_SHM_ATTACH, (u64)(s64)id, (u64)(uintptr_t)out_vaddr, 0, 0);
}

s32 sys_shm_detach(void *vaddr) {
    return (s32)syscall4(SYS_SHM_DETACH, (u64)(uintptr_t)vaddr, 0, 0, 0);
}

s32 sys_shm_destroy(s32 id) {
    return (s32)syscall4(SYS_SHM_DESTROY, (u64)(s64)id, 0, 0, 0);
}

s32 sys_net_status(sys_net_status_t *out) {
    return (s32)syscall4(SYS_NET_STATUS, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_net_dhcp(u32 timeout_ms) {
    return (s32)syscall4(SYS_NET_DHCP, timeout_ms, 0, 0, 0);
}

s32 sys_net_resolve(const char *host, u32 *out_ip) {
    return (s32)syscall4(SYS_NET_RESOLVE, (u64)(uintptr_t)host, (u64)(uintptr_t)out_ip, 0, 0);
}

s32 sys_net_ping(u32 ip, u32 timeout_ms, u32 *out_rtt_ms) {
    return (s32)syscall4(SYS_NET_PING, ip, timeout_ms, (u64)(uintptr_t)out_rtt_ms, 0);
}

s32 sys_tcp_connect(u32 ip, u16 port) {
    return (s32)syscall4(SYS_TCP_CONNECT, ip, port, 0, 0);
}

s32 sys_tcp_send(s32 h, const void *buf, u32 len) {
    return (s32)syscall4(SYS_TCP_SEND, (u64)(s64)h, (u64)(uintptr_t)buf, len, 0);
}

s32 sys_tcp_recv(s32 h, void *buf, u32 maxlen, u32 timeout_ms) {
    return (s32)syscall4(SYS_TCP_RECV, (u64)(s64)h, (u64)(uintptr_t)buf, maxlen, timeout_ms);
}

s32 sys_tcp_close(s32 h) {
    return (s32)syscall4(SYS_TCP_CLOSE, (u64)(s64)h, 0, 0, 0);
}

s32 sys_fs_write_file(const char *name, const void *data, u32 len) {
    return (s32)syscall4(SYS_FS_WRITE_FILE, (u64)(uintptr_t)name, (u64)(uintptr_t)data, len, 0);
}

s32 sys_fs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    return (s32)syscall4(SYS_FS_READ_FILE, (u64)(uintptr_t)name, (u64)(uintptr_t)buf, bufsize,
                          (u64)(uintptr_t)out_size);
}

s32 sys_fs_stat(const char *name, sys_fs_stat_t *out) {
    return (s32)syscall4(SYS_FS_STAT, (u64)(uintptr_t)name, (u64)(uintptr_t)out, 0, 0);
}

s32 sys_fs_pwd(char *buf, u32 bufsize) {
    return (s32)syscall4(SYS_FS_PWD, (u64)(uintptr_t)buf, bufsize, 0, 0);
}

s32 sys_fs_cd(const char *name) {
    return (s32)syscall4(SYS_FS_CD, (u64)(uintptr_t)name, 0, 0, 0);
}

s32 sys_fs_mkdir(const char *name) {
    return (s32)syscall4(SYS_FS_MKDIR, (u64)(uintptr_t)name, 0, 0, 0);
}

s32 sys_fs_rm(const char *name) {
    return (s32)syscall4(SYS_FS_RM, (u64)(uintptr_t)name, 0, 0, 0);
}

s32 sys_fs_list(sys_fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    return (s32)syscall4(SYS_FS_LIST, (u64)(uintptr_t)out, max_entries, (u64)(uintptr_t)out_count, 0);
}

s32 sys_ps_list(sys_task_info_t *out, u32 max_entries, u32 *out_count) {
    return (s32)syscall4(SYS_PS_LIST, (u64)(uintptr_t)out, max_entries, (u64)(uintptr_t)out_count, 0);
}

s32 sys_sleep(u32 ms) {
    return (s32)syscall4(SYS_SLEEP, ms, 0, 0, 0);
}

s32 sys_power_reboot(void) {
    return (s32)syscall4(SYS_POWER_REBOOT, 0, 0, 0, 0);
}

s32 sys_power_shutdown(void) {
    return (s32)syscall4(SYS_POWER_SHUTDOWN, 0, 0, 0, 0);
}

s32 sys_users_whoami(sys_user_info_t *out) {
    return (s32)syscall4(SYS_USERS_WHOAMI, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_users_list(sys_user_info_t *out, u32 max_entries, u32 *out_count) {
    return (s32)syscall4(SYS_USERS_LIST, (u64)(uintptr_t)out, max_entries, (u64)(uintptr_t)out_count, 0);
}

s32 sys_users_authenticate(const char *name, const char *password) {
    return (s32)syscall4(SYS_USERS_AUTHENTICATE, (u64)(uintptr_t)name, (u64)(uintptr_t)password, 0, 0);
}

s32 sys_users_read_password(char *buf, u32 maxlen) {
    return (s32)syscall4(SYS_USERS_READ_PASSWORD, (u64)(uintptr_t)buf, maxlen, 0, 0);
}

s32 sys_users_passwd(const char *target, const char *current_pass_or_null, const char *new_pass) {
    return (s32)syscall4(SYS_USERS_PASSWD, (u64)(uintptr_t)target, (u64)(uintptr_t)current_pass_or_null,
                          (u64)(uintptr_t)new_pass, 0);
}

s32 sys_users_add(const char *name, const char *password, bool_t is_root) {
    return (s32)syscall4(SYS_USERS_ADD, (u64)(uintptr_t)name, (u64)(uintptr_t)password, is_root, 0);
}

s32 sys_users_del(const char *name) {
    return (s32)syscall4(SYS_USERS_DEL, (u64)(uintptr_t)name, 0, 0, 0);
}

s32 sys_users_su(const char *target, const char *password_or_null) {
    return (s32)syscall4(SYS_USERS_SU, (u64)(uintptr_t)target, (u64)(uintptr_t)password_or_null, 0, 0);
}

s32 sys_users_logout(void) {
    return (s32)syscall4(SYS_USERS_LOGOUT, 0, 0, 0, 0);
}

s32 sys_storage_list(sys_storage_info_t *out, u32 max_entries, u32 *out_count) {
    return (s32)syscall4(SYS_STORAGE_LIST, (u64)(uintptr_t)out, max_entries, (u64)(uintptr_t)out_count, 0);
}

s32 sys_usb_list(sys_usb_info_t *out, u32 max_entries, u32 *out_count) {
    return (s32)syscall4(SYS_USB_LIST, (u64)(uintptr_t)out, max_entries, (u64)(uintptr_t)out_count, 0);
}

s32 sys_usb_mouse_state(sys_mouse_state_t *out) {
    return (s32)syscall4(SYS_USB_MOUSE_STATE, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_rtc_read(sys_rtc_time_t *out) {
    return (s32)syscall4(SYS_RTC_READ, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_fs_mount(u32 drive_index) {
    return (s32)syscall4(SYS_FS_MOUNT, drive_index, 0, 0, 0);
}

s32 sys_fs_format(u32 drive_index, u32 fs_type) {
    return (s32)syscall4(SYS_FS_FORMAT, drive_index, fs_type, 0, 0);
}

s32 sys_fs_chmod(const char *name, u32 mode) {
    return (s32)syscall4(SYS_FS_CHMOD, (u64)(uintptr_t)name, mode, 0, 0);
}

s32 sys_fs_chown(const char *name, u32 new_uid) {
    return (s32)syscall4(SYS_FS_CHOWN, (u64)(uintptr_t)name, new_uid, 0, 0);
}

s32 sys_fs_backend_info(sys_fs_backend_info_t *out) {
    return (s32)syscall4(SYS_FS_BACKEND_INFO, (u64)(uintptr_t)out, 0, 0, 0);
}

s32 sys_net_set_static(u32 ip, u32 netmask, u32 gateway, u32 dns) {
    return (s32)syscall4(SYS_NET_SET_STATIC, ip, netmask, gateway, dns);
}

s32 sys_container_spawn(const char *manifest_path, u32 argc, char **argv) {
    return (s32)syscall4(SYS_CONTAINER_SPAWN, (u64)(uintptr_t)manifest_path, argc, (u64)(uintptr_t)argv, 0);
}

s32 sys_irq_wait(u32 irq) {
    return (s32)syscall4(SYS_IRQ_WAIT, irq, 0, 0, 0);
}

s32 sys_netbridge_tx_dequeue(void *buf, u32 maxlen) {
    return (s32)syscall4(SYS_NETBRIDGE_TX_DEQUEUE, (u64)(uintptr_t)buf, maxlen, 0, 0);
}

s32 sys_netbridge_rx_enqueue(const void *buf, u32 len) {
    return (s32)syscall4(SYS_NETBRIDGE_RX_ENQUEUE, (u64)(uintptr_t)buf, len, 0, 0);
}

/* ---- Фаза 5, подшаг 5.5 (см. include/sysabi.h за SYS_CLEAR/SYS_FS_USE_RAMFS) ---- */
s32 sys_clear(void) {
    return (s32)syscall4(SYS_CLEAR, 0, 0, 0, 0);
}

s32 sys_fs_use_ramfs(void) {
    return (s32)syscall4(SYS_FS_USE_RAMFS, 0, 0, 0, 0);
}

s32 sys_disk_read_raw(u32 drive_index, u32 lba, u32 count, void *buf) {
    return (s32)syscall4(SYS_DISK_READ_RAW, drive_index, lba, count, (u64)(uintptr_t)buf);
}

s32 sys_disk_write_raw(u32 drive_index, u32 lba, u32 count, const void *buf) {
    return (s32)syscall4(SYS_DISK_WRITE_RAW, drive_index, lba, count, (u64)(uintptr_t)buf);
}

s32 sys_boot_asset_read(u32 asset_id, void *buf, u32 bufsize, u32 *out_size) {
    return (s32)syscall4(SYS_BOOT_ASSET_READ, asset_id, (u64)(uintptr_t)buf, bufsize, (u64)(uintptr_t)out_size);
}

/* ATOMIC_PLAN.md, Фаза 7 -- см. общий комментарий у SYS_BOOT_ASSET_WRITE_TO_DISK/
   VERIFY_ON_DISK, include/sysabi.h. */
s32 sys_boot_asset_write_to_disk(u32 asset_id, u32 drive_index, u32 lba_start, u32 region_sectors) {
    return (s32)syscall4(SYS_BOOT_ASSET_WRITE_TO_DISK, asset_id, drive_index, lba_start, region_sectors);
}

s32 sys_boot_asset_verify_on_disk(u32 asset_id, u32 drive_index, u32 lba_start) {
    return (s32)syscall4(SYS_BOOT_ASSET_VERIFY_ON_DISK, asset_id, drive_index, lba_start, 0);
}

/* ATOMIC_PLAN.md, Фаза 6 -- см. общий комментарий у SYS_BOOT_CONFIRM,
   include/sysabi.h -- добавлена сейчас, вместе с Фазой 7, чисто для
   полноты ABI (ядро само вызывает bootconfirm_mark_ok() напрямую, эта
   обёртка раньше была не нужна никакому userspace-коду). */
s32 sys_boot_confirm(void) {
    return (s32)syscall4(SYS_BOOT_CONFIRM, 0, 0, 0, 0);
}

s32 sys_fs_format_home_encrypted(u32 drive_index, u32 slot_body_lba_start,
                                  u32 slot_body_sectors, const u8 *master_key) {
    return (s32)syscall4(SYS_FS_FORMAT_HOME_ENCRYPTED, drive_index, slot_body_lba_start,
                          slot_body_sectors, (u64)(uintptr_t)master_key);
}

#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "isr.h"

/* process.h -- настоящие процессы поверх task_t + address_space_t (см.
 * include/task.h, include/paging.h). Заменяет собой историческую пару
 * kernel/usermode.{c,s} (один-единственный синхронный "поход в ring 3
 * и обратно" на всю систему, без отдельных таблиц страниц) -- теперь
 * sandboxed-приложение это ОБЫЧНАЯ задача планировщика, которая просто
 * умеет исполняться в ring 3, а kernel/loader.c лишь ждёт её завершения
 * так же, как раньше ждал usermode_run(). */

/* Принимает готовый ELF64-образ в буфере ядра (elf_data/elf_size --
   см. loader_run_file_sandboxed()/sys_execve() в kernel/syscall.c,
   которые читают файл через fs_read_file() перед вызовом сюда) и
   запускает его как отдельный процесс:
   собственное адресное пространство, собственный (растущий по
   требованию) стек и куча, ring 3. Блокирует вызывающую задачу (обычным
   task_yield() в цикле -- вызывающая задача остаётся в очереди
   планировщика и не мешает остальным) до завершения процесса и
   возвращает его код возврата, либо -1 при нехватке памяти. Совместима
   по поведению с прежней usermode_run(): 'run' в shell/shell.c не
   пришлось менять. */
s32 process_run_sandboxed(const void *elf_data, u32 elf_size, u32 argc, char **argv);

/* То же самое, что process_run_sandboxed(), но с явно заданной маской
   CAP_* (см. include/container.h) вместо CAP_ALL по умолчанию --
   process_run_sandboxed() -- это ровно process_run_sandboxed_caps() с
   caps=CAP_ALL, io_base=0, io_len=0 (см. kernel/process.c). Используется
   container_spawn() (kernel/container.c), чтобы посадить процесс на
   права, разрешённые манифестом контейнера, а не на "всё можно".
   io_base/io_len -- диапазон портов ввода-вывода, разрешённый из ring 3
   (см. CAP_IO, include/container.h); игнорируются (трактуются как
   io_len=0, т.е. нет доступа), если в caps нет CAP_IO -- проверка и
   обнуление на стороне вызывающего уже сделаны в
   container_manifest_parse() (kernel/container.c), здесь -- просто
   сквозная передача в task_t (см. include/task.h::io_port_base/io_port_len).
   irq_line -- аналогично, но для CAP_IRQ (см. include/irqbridge.h);
   CONTAINER_IRQ_NONE (include/container.h), если CAP_IRQ не запрошен. */
s32 process_run_sandboxed_caps(const void *elf_data, u32 elf_size, u32 argc, char **argv,
                                u32 caps, u16 io_base, u16 io_len, u8 irq_line);

/* Заменяет образ текущего процесса новым ELF-образом (execve). */
s64 process_execve(const void *elf_data, u32 elf_size, u32 argc, char **argv, registers_t *regs);

/* Завершает ТЕКУЩУЮ (вызывающую) процессную задачу с кодом code --
   аналог _exit() в обычных ОС. Вызывается из kernel/syscall.c (SYS_EXIT)
   и из kernel/isr.c (принудительное завершение по необработанному
   исключению/#PF в ring 3, см. include/paging.h). Не возвращается. */
void process_exit(s32 code) __attribute__((noreturn));

/* fork(): клонирует ТЕКУЩИЙ процесс (адресное пространство -- через
   copy-on-write, см. as_clone_cow()) в новую задачу планировщика,
   которая продолжит исполнение с ТОЙ ЖЕ точки (regs -- это снимок
   регистров ring 3 на момент системного вызова SYS_FORK, см.
   kernel/syscall.c), но с rax=0. Возвращает потомку 0 (по соглашению
   POSIX-подобных fork(), хотя формально это просто pid, который сам
   syscall_isr_handler кладёт в regs->rax родителя) -- родителю: pid
   потомка, либо отрицательное значение при нехватке памяти. Годится
   только если текущая задача сама является процессом (regs пришли из
   ring 3) -- для потоков ядра нет смысла. */
s64 process_fork(const registers_t *regs);

/* waitpid(): ждёт завершения потомка, порождённого нашим process_fork()
   (см. её реализацию в process.c за точной семантикой pid/ECHILD).
   Блокирует вызывающего (через task_yield() в цикле, как и
   process_run_sandboxed() выше) до тех пор, пока подходящий потомок не
   станет TERMINATED, либо возвращает -1 немедленно, если такого
   потомка нет вовсе. out_code, если не 0, получает exit_code найденного
   потомка. Возвращает id завершившегося потомка либо -1. */
s64 process_waitpid(s32 pid, s32 *out_code);

#endif

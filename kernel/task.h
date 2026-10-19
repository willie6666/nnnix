/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TASK_H
#define NNNIX_TASK_H
#include <stdint.h>

/* Preemptive scheduler: fixed priorities (0 = highest, 7 = idle), round-robin
 * between equal priorities with a 10 ms time slice, 1 ms tick. */
#define TASK_MAX        12           /* table of pointers: idle, fsd, kd, 4 shells, workers, pipeline stages */
#define TASK_PRIO_IDLE  7
#define TASK_REDZONE    128u         /* a saved sp this close to the stack bottom is an overflow */
#define ETIMEDOUT       110
#define EIO             5            /* net_recv: a failed transfer */
#define EINTR_TASK      4
#define ESRCH           3
#define ECHILD          10

enum task_state { TASK_UNUSED, TASK_READY, TASK_SLEEPING, TASK_BLOCKED, TASK_ZOMBIE };

struct task;
struct tty;
struct file;
#define TASK_FDS 8                   /* per-task file descriptors */
struct waitq { struct task *head, *tail; };

struct task {
    uint32_t *sp;                    /* saved stack pointer: must stay the first member */
    uint32_t *stack;                 /* lowest address of the stack allocation */
    uint32_t stack_size;
    const char *name;
    uint16_t pid;
    uint8_t prio, state, detached, slice;
    uint8_t preempted, yielded;      /* round-robin bookkeeping */
    uint8_t user;                    /* started by a program (spawn): may be killed */
    uint8_t killed;                  /* exits at a safe kernel/scheduling boundary */
    uint8_t interruptible;           /* current wait may be cut short by a kill */
    uint16_t ppid;                   /* parent, for wait */
    void *owned;                     /* kmalloc'd with the task, freed when it is reaped */
    int32_t wait_result, exit_code;
    uint32_t wake_at;                /* 0: no timeout */
    uint32_t last_run, cpu_ms;
    struct waitq *waitq;             /* queue the task is blocked on */
    struct task *wait_next;
    struct waitq exit_wq;
    struct tty *tty;                 /* controlling terminal: fd 0/1/2; inherited by children */
    struct file *fds[TASK_FDS];      /* open files; unset 0-2 mean the terminal */
    char *cwd;                       /* kmalloc'd; 0 means "/"; inherited by children */
    struct loaded_image *image;      /* the loaded program this task runs code of, or 0 (kernel/xip.h) */
};

struct task_info {
    char name[12];                   /* copied, cut to 11 characters */
    uint16_t pid;
    uint8_t prio, state;
    uint32_t stack_size, stack_used, cpu_ms;
};

void sched_init(void);
void sched_start(void) __attribute__((noreturn));
int  sched_running(void);
void sched_tick(void);               /* from SysTick */

struct task *task_spawn(const char *name, void (*entry)(void *), void *arg,
                        uint32_t stack_bytes, uint8_t prio, int flags);
void task_start(struct task *t);       /* make a TASK_PAUSED task runnable */
#define TASK_DETACHED  1                 /* task_spawn flags: nobody joins it */
#define TASK_LONG_LIVED 2                /* stack from the top of the heap (login shells) */
#define TASK_PAUSED    4                 /* created stopped: the caller sets it up, then task_start() */
struct task *task_self(void);
void task_yield(void);
void task_sleep(uint32_t ms);
/* Sleep in a task, busy-wait before the scheduler starts or in an interrupt. */
void msleep(uint32_t ms);
void task_exit(int code) __attribute__((noreturn));
int  task_join(struct task *t, uint32_t *cpu_ms);   /* wait for exit, free it, return the exit code */
int  task_list(struct task_info *out, int max, int skip);   /* skip the first `skip` tasks */
/* Highest stack use seen per configured stack size, ended tasks included. */
int  task_stack_peaks(uint16_t sizes[], uint16_t used[], int max);
void task_report_leftovers(void);    /* kernel daemon: log leftover memory freed at reaping */

/* A killed user task leaves at a system call or when the scheduler has saved
 * a frame in program code. Kernel operations finish before exiting, so their
 * locks are not abandoned. Interruptible waits (sleep, pipes, terminal input,
 * wait) end early with -EINTR. When a task exits, its
 * children are killed and detached, like a hang-up. */
int  task_kill(uint16_t pid);
/* Cancel a foreground operation in a protected shell without killing it.
 * A blocking terminal read notices within its one-second input timeout. */
int  task_interrupt_tty(uint16_t pid);
/* A killed program running its own code without system calls: once it is
 * preempted there, it is made to exit (130) when it next runs, its frames
 * dropped. 1 while the task is still there (call again), 0 once it is gone. */
int  task_end_now(uint16_t pid);
int  task_wait_pid(uint16_t pid, int *status, uint32_t timeout_ms);  /* pid, 0 if still running, <0 error */
int  task_tty_users(const struct tty *tty);                          /* tasks using this terminal */
int  sched_block_interruptible(struct waitq *q, uint32_t timeout_ms, uint32_t primask);
uint32_t sched_switches(void);

/* Low-level blocking used by sync.c. Call with interrupts saved by irq_save();
 * the state is restored before returning. Returns wait_result (0 or -ETIMEDOUT). */
int  sched_block(struct waitq *q, uint32_t timeout_ms, uint32_t primask);
void sched_wake(struct task *t, int result);          /* interrupts disabled */
struct task *waitq_pop(struct waitq *q);              /* interrupts disabled */

#endif

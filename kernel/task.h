/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TASK_H
#define NNNIX_TASK_H
#include <stdint.h>

/* Preemptive scheduler: fixed priorities (0 = highest, 7 = idle), round-robin
 * between equal priorities with a 10 ms time slice, 1 ms tick. */
#define TASK_MAX        10
#define TASK_PRIO_IDLE  7
#define TASK_REDZONE    128u         /* a saved sp this close to the stack bottom is an overflow */
#define ETIMEDOUT       110

enum task_state { TASK_UNUSED, TASK_READY, TASK_SLEEPING, TASK_BLOCKED, TASK_ZOMBIE };

struct task;
struct waitq { struct task *head, *tail; };

struct task {
    uint32_t *sp;                    /* saved stack pointer: must stay the first member */
    uint32_t *stack;                 /* lowest address of the stack allocation */
    uint32_t stack_size;
    const char *name;
    uint16_t pid;
    uint8_t prio, state, detached, slice;
    int32_t wait_result, exit_code;
    uint32_t wake_at;                /* 0: no timeout */
    uint32_t last_run, cpu_ms;
    struct waitq *waitq;             /* queue the task is blocked on */
    struct task *wait_next;
    struct waitq exit_wq;
};

struct task_info {
    const char *name;
    uint16_t pid;
    uint8_t prio, state;
    uint32_t stack_size, stack_used, cpu_ms;
};

void sched_init(void);
void sched_start(void) __attribute__((noreturn));
int  sched_running(void);
void sched_tick(void);               /* from SysTick */

struct task *task_spawn(const char *name, void (*entry)(void *), void *arg,
                        uint32_t stack_bytes, uint8_t prio, int detached);
struct task *task_self(void);
void task_yield(void);
void task_sleep(uint32_t ms);
void task_exit(int code) __attribute__((noreturn));
int  task_join(struct task *t, uint32_t *cpu_ms);   /* wait for exit, free it, return the exit code */
int  task_list(struct task_info *out, int max);
uint32_t sched_switches(void);

/* Low-level blocking used by sync.c. Call with interrupts saved by irq_save();
 * the state is restored before returning. Returns wait_result (0 or -ETIMEDOUT). */
int  sched_block(struct waitq *q, uint32_t timeout_ms, uint32_t primask);
void sched_wake(struct task *t, int result);          /* interrupts disabled */
struct task *waitq_pop(struct waitq *q);              /* interrupts disabled */

#endif

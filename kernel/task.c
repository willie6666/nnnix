/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task.h"
#include "kmem.h"
#include "panic.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../arch/nuc140/arch.h"

#define SLICE_MS   10u
#define STACK_FILL 0x5354434bu       /* "STCK": painted on new stacks for high-water marks */

static struct task tasks[TASK_MAX];
void *sched_current;                 /* struct task *, read by PendSV */
static int running;
static uint16_t next_pid = 1;
static uint32_t switch_seq, switches;
static struct task *idle;

static struct task *cur(void) { return (struct task *)sched_current; }
int sched_running(void) { return running; }
struct task *task_self(void) { return running ? cur() : 0; }
uint32_t sched_switches(void) { return switches; }

/* ---- wait queues (interrupts disabled) ---- */
static void waitq_append(struct waitq *q, struct task *t)
{
    t->wait_next = 0;
    t->waitq = q;
    if (q->tail) q->tail->wait_next = t; else q->head = t;
    q->tail = t;
}

static void waitq_remove(struct waitq *q, struct task *t)
{
    struct task **link = &q->head, *prev = 0;
    while (*link && *link != t) { prev = *link; link = &(*link)->wait_next; }
    if (!*link) return;
    *link = t->wait_next;
    if (q->tail == t) q->tail = prev;
    t->waitq = 0;
    t->wait_next = 0;
}

struct task *waitq_pop(struct waitq *q)
{
    struct task *t = q->head;
    if (t) waitq_remove(q, t);
    return t;
}

void sched_wake(struct task *t, int result)
{
    if (t->state != TASK_BLOCKED && t->state != TASK_SLEEPING) return;
    if (t->waitq) waitq_remove(t->waitq, t);
    t->state = TASK_READY;
    t->wait_result = result;
    t->wake_at = 0;
    if (running && t->prio < cur()->prio) arch_pend_switch();
}

int sched_block(struct waitq *q, uint32_t timeout_ms, uint32_t primask)
{
    struct task *t = cur();
    if (!running || in_interrupt() || primask) panic("sched_block outside a task");
    t->wait_result = 0;
    t->wake_at = 0;
    if (timeout_ms) {
        t->wake_at = jiffies + timeout_ms;
        if (!t->wake_at) t->wake_at = 1;
    }
    t->state = q ? TASK_BLOCKED : TASK_SLEEPING;
    if (q) waitq_append(q, t);
    arch_pend_switch();
    irq_restore(primask);            /* PendSV runs here and switches away */
    return t->wait_result;
}

/* ---- scheduling ---- */
static int stack_ok(const struct task *t)
{
    if (!t->stack) return 1;         /* idle before the first switch-out */
    if ((uintptr_t)t->sp < (uintptr_t)t->stack + TASK_REDZONE) return 0;
    for (int i = 0; i < 8; ++i) if (t->stack[i] != STACK_FILL) return 0;
    return 1;
}

/* Highest priority first. Among equals, a task that a higher priority took the
 * CPU from resumes its slice; otherwise the one that waited longest runs. */
static struct task *pick(void)
{
    struct task *best = 0;
    for (int i = 0; i < TASK_MAX; ++i) {
        struct task *t = &tasks[i];
        if (t->state != TASK_READY) continue;
        if (!best || t->prio < best->prio ||
            (t->prio == best->prio && (t->preempted > best->preempted ||
                                       (t->preempted == best->preempted && t->last_run < best->last_run))))
            best = t;
    }
    return best;
}

/* Called from PendSV with the outgoing context already saved. */
void sched_switch(void)
{
    uint32_t s = irq_save();
    struct task *prev = cur();
    if (!stack_ok(prev)) panic("stack overflow in %s", prev->name);
    struct task *next = pick();
    if (next != prev) {
        /* Still runnable with slice left and not yielding: it was preempted. */
        prev->preempted = prev->state == TASK_READY && prev->slice && !prev->yielded;
        prev->yielded = 0;
        if (!next->preempted) next->slice = SLICE_MS;
        next->preempted = 0;
        next->last_run = ++switch_seq;
        switches++;
        sched_current = next;
    }
    irq_restore(s);
}

void sched_tick(void)
{
    if (!running) return;
    struct task *c = cur();
    int resched = 0;
    c->cpu_ms++;
    for (int i = 0; i < TASK_MAX; ++i) {
        struct task *t = &tasks[i];
        if ((t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) && t->wake_at &&
            (int32_t)(jiffies - t->wake_at) >= 0) {
            sched_wake(t, -ETIMEDOUT);       /* pends a switch if it outranks the current task */
        }
    }
    if (c->slice && --c->slice == 0) {
        for (int i = 0; i < TASK_MAX; ++i)
            if (&tasks[i] != c && tasks[i].state == TASK_READY && tasks[i].prio == c->prio) resched = 1;
        if (!resched) c->slice = SLICE_MS;          /* nobody to share with: keep going */
    }
    if (resched) arch_pend_switch();
}

/* ---- task lifetime ---- */
static void task_return(void) { task_exit(0); }

struct task *task_spawn(const char *name, void (*entry)(void *), void *arg,
                        uint32_t stack_bytes, uint8_t prio, int detached)
{
    stack_bytes = (stack_bytes + 7u) & ~7u;
    if (stack_bytes < TASK_REDZONE + 128u || prio > TASK_PRIO_IDLE) return 0;
    uint32_t *stack = kmalloc(stack_bytes);
    if (!stack) return 0;
    for (uint32_t i = 0; i < stack_bytes / 4u; ++i) stack[i] = STACK_FILL;
    uint32_t s = irq_save();
    struct task *t = 0;
    for (int i = 0; i < TASK_MAX && !t; ++i) if (tasks[i].state == TASK_UNUSED) t = &tasks[i];
    if (!t) { irq_restore(s); kfree(stack); return 0; }
    *t = (struct task){ .stack = stack, .stack_size = stack_bytes, .name = name, .pid = next_pid++,
                        .prio = prio, .detached = (uint8_t)detached, .slice = SLICE_MS };
    t->sp = arch_task_stack_init(stack + stack_bytes / 4u, entry, arg, task_return);
    t->tty = running ? cur()->tty : 0;
    if (running && cur()->cwd) {                 /* inherit the working directory */
        t->cwd = kmalloc(strlen(cur()->cwd) + 1);
        if (t->cwd) strcpy(t->cwd, cur()->cwd);
    }
    t->state = TASK_READY;
    if (running && prio < cur()->prio) arch_pend_switch();
    irq_restore(s);
    return t;
}

void task_exit(int code)
{
    vfs_task_exit();                 /* close files while this task can still block */
    uint32_t s = irq_save();
    struct task *t = cur();
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    struct task *w;
    while ((w = waitq_pop(&t->exit_wq))) sched_wake(w, 0);
    arch_pend_switch();
    irq_restore(s);
    for (;;) { }
}

static void task_free(struct task *t)
{
    uint32_t *stack = t->stack;
    char *cwd = t->cwd;
    t->state = TASK_UNUSED;
    t->stack = 0;
    t->cwd = 0;
    kfree(stack);
    kfree(cwd);
}

int task_join(struct task *t, uint32_t *cpu_ms)
{
    uint32_t s = irq_save();
    while (t->state != TASK_ZOMBIE) {
        sched_block(&t->exit_wq, 0, s);
        s = irq_save();
    }
    int code = t->exit_code;
    if (cpu_ms) *cpu_ms = t->cpu_ms;
    task_free(t);
    irq_restore(s);
    return code;
}

void task_yield(void)
{
    uint32_t s = irq_save();
    cur()->last_run = ++switch_seq;  /* go behind equal-priority peers */
    cur()->yielded = 1;
    arch_pend_switch();
    irq_restore(s);
}

void msleep(uint32_t ms)
{
    if (running && !in_interrupt()) task_sleep(ms);
    else delay_ms(ms);
}

void task_sleep(uint32_t ms)
{
    uint32_t s = irq_save();
    sched_block(0, ms ? ms : 1, s);
}

int task_list(struct task_info *out, int max, int skip)
{
    int n = 0;
    uint32_t s = irq_save();
    for (int i = 0; i < TASK_MAX && n < max; ++i) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || skip-- > 0) continue;
        uint32_t untouched = 0;
        if (t->stack) while (untouched < t->stack_size / 4u && t->stack[untouched] == STACK_FILL) untouched++;
        out[n++] = (struct task_info){ t->name, t->pid, t->prio, t->state, t->stack_size,
                                       t->stack ? t->stack_size - untouched * 4u : 0, t->cpu_ms };
    }
    irq_restore(s);
    return n;
}

/* ---- idle ---- */
static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < TASK_MAX; ++i) {
            struct task *t = &tasks[i];
            uint32_t s = irq_save();
            int reap = t->state == TASK_ZOMBIE && t->detached;
            if (reap) task_free(t);
            irq_restore(s);
        }
        __asm volatile("wfi");
    }
}

void sched_init(void)
{
    arch_sched_init();
    idle = task_spawn("idle", idle_main, 0, 384, TASK_PRIO_IDLE, 0);
    if (!idle) panic("cannot create idle task");
}

void sched_start(void)
{
    irq_disable();
    struct task *first = pick();
    first->last_run = ++switch_seq;
    sched_current = first;
    running = 1;
    arch_start_first_task(first->sp);
}

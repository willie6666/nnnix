/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task.h"
#include "kmem.h"
#include "klog.h"
#include "panic.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../arch/nuc140/arch.h"

#define SLICE_MS   10u
#define STACK_FILL 0x5354434bu       /* "STCK": painted on new stacks for high-water marks */

/* A task's struct lives in the same block as its stack, just above the stack
 * top (where an overflow does not reach it first); the table holds pointers. */
static struct task *tasks[TASK_MAX];
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

int sched_block_interruptible(struct waitq *q, uint32_t timeout_ms, uint32_t primask)
{
    struct task *t = cur();
    if (t->killed) { irq_restore(primask); return -EINTR_TASK; }
    t->interruptible = 1;
    int r = sched_block(q, timeout_ms, primask);
    t->interruptible = 0;
    return r;
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
        struct task *t = tasks[i];
        if (!t || t->state != TASK_READY) continue;
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
        struct task *t = tasks[i];
        if (t && (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) && t->wake_at &&
            (int32_t)(jiffies - t->wake_at) >= 0) {
            sched_wake(t, -ETIMEDOUT);       /* pends a switch if it outranks the current task */
        }
    }
    if (c->slice && --c->slice == 0) {
        for (int i = 0; i < TASK_MAX; ++i)
            if (tasks[i] && tasks[i] != c && tasks[i]->state == TASK_READY && tasks[i]->prio == c->prio) resched = 1;
        if (!resched) c->slice = SLICE_MS;          /* nobody to share with: keep going */
    }
    if (resched) arch_pend_switch();
}

/* ---- task lifetime ---- */
static struct task *find(uint16_t pid);

/* Interrupts off. PIDs wrap after 65535: skip 0 ("none") and any PID still in
 * the table, zombies included, so wait, kill and heap ownership stay unique. */
static uint16_t new_pid(void)
{
    uint16_t pid;
    do pid = next_pid++; while (!pid || find(pid));
    return pid;
}

static void task_return(void) { task_exit(0); }

struct task *task_spawn(const char *name, void (*entry)(void *), void *arg,
                        uint32_t stack_bytes, uint8_t prio, int flags)
{
    stack_bytes = (stack_bytes + 7u) & ~7u;
    if (stack_bytes < TASK_REDZONE + 128u || prio > TASK_PRIO_IDLE) return 0;
    uint32_t size = stack_bytes + (uint32_t)((sizeof(struct task) + 7u) & ~7u);
    uint32_t *stack = (flags & TASK_LONG_LIVED) ? kmalloc_high(size) : kmalloc(size);
    if (!stack) return 0;
    for (uint32_t i = 0; i < stack_bytes / 4u; ++i) stack[i] = STACK_FILL;
    struct task *t = (struct task *)((char *)stack + stack_bytes);
    uint32_t s = irq_save();
    int slot = 0;
    while (slot < TASK_MAX && tasks[slot]) ++slot;
    if (slot == TASK_MAX) { irq_restore(s); kfree(stack); return 0; }
    uint16_t pid = new_pid();                   /* before t is in the table */
    tasks[slot] = t;
    *t = (struct task){ .stack = stack, .stack_size = stack_bytes, .name = name, .pid = pid,
                        .prio = prio, .detached = (uint8_t)(flags & TASK_DETACHED), .slice = SLICE_MS,
                        .ppid = running ? cur()->pid : 0 };
    t->sp = arch_task_stack_init(stack + stack_bytes / 4u, entry, arg, task_return);
    t->tty = running ? cur()->tty : 0;
    if (running && cur()->cwd) {                 /* inherit the working directory */
        uint32_t n = (uint32_t)strlen(cur()->cwd) + 1u;
        t->cwd = (flags & TASK_LONG_LIVED) ? kmalloc_high(n) : kmalloc(n);
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
    for (int i = 0; i < TASK_MAX; ++i) {          /* hang up the children */
        struct task *c = tasks[i];
        if (!c || c == t || c->ppid != t->pid) continue;
        c->detached = 1;
        c->ppid = 0;
        if (c->user && c->state != TASK_ZOMBIE) {
            c->killed = 1;
            if (c->interruptible) sched_wake(c, -EINTR_TASK);
        }
    }
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    struct task *w;
    while ((w = waitq_pop(&t->exit_wq))) sched_wake(w, 0);
    arch_pend_switch();
    irq_restore(s);
    for (;;) { }
}

/* Frees the struct too: no pointer to t may be used afterwards. Called with
 * interrupts off. Program allocations the task left behind are freed while its
 * pid is still taken, so a new task cannot get that pid (and lose its own
 * blocks). Returns how many there were, to be logged with interrupts on. */
static int task_free(struct task *t)
{
    uint32_t *block = t->stack;
    char *cwd = t->cwd;
    void *owned = t->owned;
    int left = kmem_free_owned(t->pid);
    for (int i = 0; i < TASK_MAX; ++i) if (tasks[i] == t) tasks[i] = 0;
    kfree(cwd);
    kfree(owned);
    kfree(block);
    return left;
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
    uint16_t pid = t->pid;
    int left = task_free(t);
    irq_restore(s);
    if (left) kprintf("pid %u: freed %d blocks it left allocated\n", pid, left);
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
    sched_block_interruptible(0, ms ? ms : 1, s);
}

static struct task *find(uint16_t pid)
{
    for (int i = 0; i < TASK_MAX; ++i)
        if (tasks[i] && tasks[i]->pid == pid) return tasks[i];
    return 0;
}

int task_kill(uint16_t pid)
{
    uint32_t s = irq_save();
    struct task *t = find(pid);
    int r = 0;
    if (!t || t->state == TASK_ZOMBIE) r = -ESRCH;
    else if (!t->user) r = -1;                   /* kernel tasks cannot be killed */
    else {
        t->killed = 1;
        if (t->interruptible) sched_wake(t, -EINTR_TASK);
    }
    irq_restore(s);
    return r;
}

int task_wait_pid(uint16_t pid, int *status, uint32_t timeout_ms)
{
    uint32_t s = irq_save();
    struct task *t = find(pid), *self = cur();
    if (!t || t->ppid != self->pid || t->detached) { irq_restore(s); return -ECHILD; }
    if (t->state != TASK_ZOMBIE && timeout_ms) {
        int r = sched_block_interruptible(&t->exit_wq, timeout_ms == UINT32_MAX ? 0 : timeout_ms, s);
        s = irq_save();
        if (r == -EINTR_TASK) { irq_restore(s); return r; }
    }
    if (t->state != TASK_ZOMBIE) { irq_restore(s); return 0; }
    irq_restore(s);
    int code = task_join(t, 0);
    if (status) *status = code;
    return pid;
}

int task_tty_users(const struct tty *tty)
{
    int n = 0;
    uint32_t s = irq_save();
    for (int i = 0; i < TASK_MAX; ++i)
        if (tasks[i] && tasks[i]->state != TASK_ZOMBIE && tasks[i]->tty == tty) ++n;
    irq_restore(s);
    return n;
}

int task_list(struct task_info *out, int max, int skip)
{
    int n = 0;
    uint32_t s = irq_save();
    for (int i = 0; i < TASK_MAX && n < max; ++i) {
        struct task *t = tasks[i];
        if (!t || skip-- > 0) continue;
        uint32_t untouched = 0;
        if (t->stack) while (untouched < t->stack_size / 4u && t->stack[untouched] == STACK_FILL) untouched++;
        /* A copy of the name: a program's name lives in its argv block, which is
         * freed when it is reaped, possibly before the caller prints the list. */
        struct task_info *o = &out[n++];
        *o = (struct task_info){ .pid = t->pid, .prio = t->prio, .state = t->state, .stack_size = t->stack_size,
                                 .stack_used = t->stack ? t->stack_size - untouched * 4u : 0, .cpu_ms = t->cpu_ms };
        uint32_t k = 0;
        while (k + 1 < sizeof o->name && t->name[k]) { o->name[k] = t->name[k]; ++k; }
        o->name[k] = 0;
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
            uint32_t s = irq_save();
            struct task *t = tasks[i];
            if (t && t->state == TASK_ZOMBIE && t->detached) task_free(t);   /* idle must not log (block) */
            irq_restore(s);
        }
        __asm volatile("wfi");
    }
}

void sched_init(void)
{
    arch_sched_init();
    idle = task_spawn("idle", idle_main, 0, 256, TASK_PRIO_IDLE, 0);    /* uses about 80 B */
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

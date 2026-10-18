/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task.h"
#include "xip.h"
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
    if (!stack_ok(prev)) {
        /* Which check failed tells a deep call (sp in the red zone) from a write
         * into the stack's bottom from below; pc and lr say where it ran. */
        const uint32_t *f = prev->sp;
        uint32_t used = (uint32_t)((uintptr_t)prev->stack + prev->stack_size - (uintptr_t)f);
        int deep = (uintptr_t)f < (uintptr_t)prev->stack + TASK_REDZONE;
        uint32_t nb[5];
        if (kmem_neighbours(prev->stack, nb))        /* who sits below: on the serial log */
            kprintf("stack %08x [%08x %08x] %08x %08x %08x; below %08x size %u tag %08x\n",
                    (unsigned)(uintptr_t)prev->stack, nb[3], nb[4], prev->stack[0], prev->stack[1],
                    prev->stack[2], nb[0], nb[1], nb[2]);
        else
            kprintf("stack %08x: heap walk failed\n", (unsigned)(uintptr_t)prev->stack);
        /* 47 characters fit the panic record: "stack overflow NAME USED pc PC lr LR",
         * USED marked '!' when sp is fine and only the painted bottom was hit. */
        panic("stack overflow %s %u%s pc %x lr %x", prev->name, used, deep ? "" : "!",
              (uintptr_t)f >= (uintptr_t)prev->stack + 64u ? f[14] : 0u,
              (uintptr_t)f >= (uintptr_t)prev->stack + 64u ? f[13] : 0u);
    }
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
    /* The working directory is inherited: without room for it the spawn fails,
     * rather than the task quietly running in "/". */
    char *cwd = 0;
    if (running && cur()->cwd) {
        uint32_t n = (uint32_t)strlen(cur()->cwd) + 1u;
        cwd = (flags & TASK_LONG_LIVED) ? kmalloc_high(n) : kmalloc(n);
        if (!cwd) { kfree(stack); return 0; }
        strcpy(cwd, cur()->cwd);
    }
    for (uint32_t i = 0; i < stack_bytes / 4u; ++i) stack[i] = STACK_FILL;
    struct task *t = (struct task *)((char *)stack + stack_bytes);
    uint32_t s = irq_save();
    int slot = 0;
    while (slot < TASK_MAX && tasks[slot]) ++slot;
    if (slot == TASK_MAX) { irq_restore(s); kfree(stack); kfree(cwd); return 0; }
    uint16_t pid = new_pid();                   /* before t is in the table */
    tasks[slot] = t;
    *t = (struct task){ .stack = stack, .stack_size = stack_bytes, .name = name, .pid = pid,
                        .prio = prio, .detached = (uint8_t)(flags & TASK_DETACHED), .slice = SLICE_MS,
                        .ppid = running ? cur()->pid : 0 };
    t->sp = arch_task_stack_init(stack + stack_bytes / 4u, entry, arg, task_return);
    t->tty = running ? cur()->tty : 0;
    t->cwd = cwd;
    t->state = (flags & TASK_PAUSED) ? TASK_SLEEPING : TASK_READY;   /* paused: no timeout, no queue */
    if (running && !(flags & TASK_PAUSED) && prio < cur()->prio) arch_pend_switch();
    irq_restore(s);
    return t;
}

void task_start(struct task *t)
{
    uint32_t s = irq_save();
    t->state = TASK_READY;
    if (running && t->prio < cur()->prio) arch_pend_switch();
    irq_restore(s);
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

/* Stack high-water marks of tasks that have ended, by configured stack size
 * (tasks of one size do the same job): what stack sizes can be trimmed to. */
#define PEAK_SLOTS 6
static struct { uint16_t size, peak; } peaks[PEAK_SLOTS];

static uint32_t stack_peak(const struct task *t)
{
    uint32_t untouched = 0;
    while (untouched < t->stack_size / 4u && t->stack[untouched] == STACK_FILL) untouched++;
    return t->stack_size - untouched * 4u;
}

static void note_peak(uint32_t size, uint32_t used)
{
    int free_slot = -1;
    for (int i = 0; i < PEAK_SLOTS; ++i) {
        if (peaks[i].size == size) { if (used > peaks[i].peak) peaks[i].peak = (uint16_t)used; return; }
        if (!peaks[i].size && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { peaks[free_slot].size = (uint16_t)size; peaks[free_slot].peak = (uint16_t)used; }
}

/* Fill words still at the bottom of t's stack, counted 32 at a time with
 * interrupts on in between: an untouched 1.6 KiB stack takes about 100 us to
 * scan, too long to hold off the 8 kHz audio interrupt. -1 if t went away. */
static int32_t untouched_words(const struct task *t, uint16_t pid)
{
    uint32_t n = 0;
    for (;;) {
        uint32_t s = irq_save();
        int alive = 0;
        for (int i = 0; i < TASK_MAX; ++i) if (tasks[i] == t && t->pid == pid) alive = 1;
        if (!alive) { irq_restore(s); return -1; }
        uint32_t words = t->stack ? t->stack_size / 4u : 0, end = n + 32u < words ? n + 32u : words;
        while (n < end && t->stack[n] == STACK_FILL) ++n;
        int done = n < end || n == words;
        irq_restore(s);
        if (done) return (int32_t)n;
    }
}

int task_stack_peaks(uint16_t sizes[], uint16_t used[], int max)
{
    for (int i = 0; i < TASK_MAX; ++i) {
        uint32_t s = irq_save();
        struct task *t = tasks[i];
        uint16_t pid = t ? t->pid : 0;
        irq_restore(s);
        int32_t u = t && t->stack ? untouched_words(t, pid) : -1;
        s = irq_save();
        if (u >= 0 && tasks[i] == t) note_peak(t->stack_size, t->stack_size - (uint32_t)u * 4u);
        irq_restore(s);
    }
    uint32_t s = irq_save();
    int n = 0;
    for (int i = 0; i < PEAK_SLOTS && n < max; ++i)
        if (peaks[i].size) { sizes[n] = peaks[i].size; used[n] = peaks[i].peak; ++n; }
    irq_restore(s);
    return n;
}

/* Leftover program allocations freed at reaping, reported by the kernel daemon
 * (task_report_leftovers): logging here would deepen whatever stack reaps the
 * task, often a waiting shell's. */
static uint16_t leftover_pid, leftover_blocks;

void task_report_leftovers(void)
{
    uint32_t s = irq_save();
    uint16_t pid = leftover_pid, blocks = leftover_blocks;
    leftover_blocks = 0;
    irq_restore(s);
    if (blocks) kprintf("pid %u: freed %u blocks it left allocated\n", pid, blocks);
}

/* Frees the struct too: no pointer to t may be used afterwards. Called with
 * interrupts on, by the only task that frees this zombie (its joiner, or idle
 * for a detached one): the parts that must be atomic take the lock briefly,
 * and the program allocations the task left behind are freed while it is
 * still in the table, so its pid stays taken and a new task cannot get it
 * (and lose its own blocks). That freeing walks the heap once per block, so
 * it runs with interrupts on between blocks: holding them off for all of it
 * delayed the 8 kHz audio interrupt by up to 60 us. */
static void task_free(struct task *t, uint32_t peak)
{
    uint32_t s = irq_save();
    note_peak(t->stack_size, peak);
    struct loaded_image *image = t->image;
    uint16_t t_pid = t->pid;
    int mine = 1;
    if (image && --image->refs) {
        /* Still used by a relative (a child it spawned, which may still run
         * until its next system call): the program's heap may be shared
         * through its globals, so it passes to that relative instead. */
        for (int i = 0; i < TASK_MAX; ++i)
            if (tasks[i] && tasks[i] != t && tasks[i]->image == image) { kmem_reown(t->pid, tasks[i]->pid); break; }
        image = 0;
        mine = 0;
    }
    irq_restore(s);
    int left = mine ? kmem_free_owned(t_pid) : 0;
    s = irq_save();
    for (int i = 0; i < TASK_MAX; ++i) if (tasks[i] == t) tasks[i] = 0;
    uint32_t *block = t->stack;
    char *cwd = t->cwd;
    void *owned = t->owned;
    if (left) { leftover_pid = t_pid; leftover_blocks = (uint16_t)(leftover_blocks + left); }
    irq_restore(s);
    kfree(cwd);
    kfree(owned);
    xip_release(image);
    kfree(block);
}

int task_join(struct task *t, uint32_t *cpu_ms)
{
    uint32_t s = irq_save();
    while (t->state != TASK_ZOMBIE) {
        sched_block(&t->exit_wq, 0, s);
        s = irq_save();
    }
    irq_restore(s);
    /* The stack scan with interrupts on (it takes tens of microseconds): a
     * zombie's stack no longer changes, and only its joiner frees it. */
    uint32_t peak = stack_peak(t);
    int code = t->exit_code;
    if (cpu_ms) *cpu_ms = t->cpu_ms;
    task_free(t, peak);
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
    for (int i = 0; i < TASK_MAX && n < max; ++i) {
        uint32_t s = irq_save();
        struct task *t = tasks[i];
        if (!t || skip-- > 0) { irq_restore(s); continue; }
        /* A copy of the name: a program's name lives in its argv block, which is
         * freed when it is reaped, possibly before the caller prints the list. */
        struct task_info *o = &out[n];
        *o = (struct task_info){ .pid = t->pid, .prio = t->prio, .state = t->state, .stack_size = t->stack_size,
                                 .cpu_ms = t->cpu_ms };
        uint32_t k = 0;
        while (k + 1 < sizeof o->name && t->name[k]) { o->name[k] = t->name[k]; ++k; }
        o->name[k] = 0;
        int has_stack = t->stack != 0;
        irq_restore(s);
        int32_t u = has_stack ? untouched_words(t, o->pid) : 0;
        if (u < 0) continue;                         /* reaped meanwhile */
        o->stack_used = has_stack ? o->stack_size - (uint32_t)u * 4u : 0;
        ++n;
    }
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
            int reap = t && t->state == TASK_ZOMBIE && t->detached;
            irq_restore(s);
            if (!reap) continue;
            task_free(t, stack_peak(t));            /* only idle frees detached zombies; it must not log (block) */
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

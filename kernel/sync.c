/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sync.h"
#include "../arch/nuc140/arch.h"

static int threaded(void) { return sched_running() && !in_interrupt(); }

void mutex_lock(struct mutex *m)
{
    if (!threaded()) return;
    uint32_t s = irq_save();
    while (m->owner) {
        sched_block(&m->wq, 0, s);
        s = irq_save();
    }
    m->owner = task_self();
    irq_restore(s);
}

int mutex_trylock(struct mutex *m)
{
    if (!threaded()) return 1;
    uint32_t s = irq_save();
    int ok = !m->owner;
    if (ok) m->owner = task_self();
    irq_restore(s);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    if (!threaded()) return;
    uint32_t s = irq_save();
    if (m->owner == task_self()) {
        m->owner = 0;
        struct task *t = waitq_pop(&m->wq);
        if (t) sched_wake(t, 0);
    }
    irq_restore(s);
}

int sem_take(struct sem *sm, uint32_t timeout_ms)
{
    uint32_t s = irq_save();
    while (sm->count <= 0) {
        int r = sched_block(&sm->wq, timeout_ms, s);
        if (r) return r;
        s = irq_save();
    }
    sm->count--;
    irq_restore(s);
    return 0;
}

void sem_give(struct sem *sm)
{
    uint32_t s = irq_save();
    sm->count++;
    struct task *t = waitq_pop(&sm->wq);
    if (t) sched_wake(t, 0);
    irq_restore(s);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SYNC_H
#define NNNIX_SYNC_H
#include <stdint.h>
#include "task.h"

/* Before the scheduler starts (single-threaded boot) and inside interrupt
 * handlers, mutex operations are no-ops. No priority inheritance. */
struct mutex { struct task *owner; struct waitq wq; };
void mutex_lock(struct mutex *m);
int  mutex_trylock(struct mutex *m);  /* 1 when acquired */
int  mutex_lock_killable(struct mutex *m);  /* 0, or SYS_EINTR when the caller is killed while waiting */
void mutex_unlock(struct mutex *m);   /* ignored unless the caller owns it */

/* Counting semaphore. sem_give is safe from interrupt handlers. */
struct sem { int32_t count; struct waitq wq; };
#define SEM_FOREVER 0u
int  sem_take(struct sem *s, uint32_t timeout_ms);   /* 0 or -ETIMEDOUT */
void sem_give(struct sem *s);

#endif

#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include <machine/archtypes.h>
#include "ktzprofile.h"

typedef struct spinlock {
	atomic_t val;
} spinlock_t;

typedef struct reentrantlock {
	spinlock_t lock;
	volatile int owner; /* Owner is <cpu>+1 so that the default value (0) is invalid. */
	volatile int n_locks; /* Number of times locked by owner. */
} reentrantlock_t;

void _reentrantlock_lock(reentrantlock_t *rl);
void _reentrantlock_unlock(reentrantlock_t *rl);

#ifndef CONFIG_SMP

#define SPINLOCK_DEFINE(name)
#define PRIVATE_SPINLOCK_DEFINE(name)
#define SPINLOCK_DECLARE(name)
#define spinlock_init(sl)
#define spinlock_lock(sl)
#define spinlock_unlock(sl)
#define reetrantlock_lock(rl)
#define reetrantlock_unlock(rl)

#else

/* SMP */
#define SPINLOCK_DEFINE(name)	spinlock_t name;
#define PRIVATE_SPINLOCK_DEFINE(name)	PRIVATE SPINLOCK_DEFINE(name)
#define SPINLOCK_DECLARE(name)	extern SPINLOCK_DEFINE(name)
#define spinlock_init(sl) do { (sl)->val = 0; } while (0)

#if CONFIG_MAX_CPUS == 1
#define spinlock_lock(sl)
#define spinlock_unlock(sl)
#define reetrantlock_lock(rl)
#define reetrantlock_unlock(rl)
#else
int arch_spinlock_lock(atomic_t * sl);
int arch_spinlock_test(atomic_t * sl);
void arch_spinlock_unlock(atomic_t * sl);
int arch_fetch_and_inc(volatile atomic_t *counter);
void *fetch_and_store(void *dest,void *val);
void *compare_and_swap(void *dest, void *expected, void *new);
#define spinlock_lock(sl)	arch_spinlock_lock(&((sl)->val))
#define spinlock_unlock(sl)	arch_spinlock_unlock(&((sl)->val))

#define reetrantlock_lock(rl)	_reentrantlock_lock(&(rl));
#define reetrantlock_unlock(rl)	_reentrantlock_unlock(&(rl));
#endif


#endif /* CONFIG_SMP */

void lock_all_procs(void);
void unlock_all_procs(void);

/* Unlock all the procs of the proc table except for `except_proc_nr`. */
void unlock_all_procs_except(int except_proc_nr);

/* To lock/unlock the BKL from asm: */
void bkl_lock(void);
void bkl_unlock(void);

#define BKL_LOCK() \
	do { \
		ktzprofile_event(KTRACE_BKL_TRY); \
		lock_all_procs(); \
		ktzprofile_event(KTRACE_BKL_ACQUIRE); \
	} while (0)

#define BKL_UNLOCK() \
	do { \
		ktzprofile_event(KTRACE_BKL_RELEASE); \
		unlock_all_procs(); \
	} while (0)

#endif /* __SPINLOCK_H__ */

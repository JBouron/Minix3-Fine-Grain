#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include <machine/archtypes.h>
#include "ktzprofile.h"
#include "bkl.h"

typedef struct spinlock {
	atomic_t val;
} spinlock_t;

#ifndef CONFIG_SMP

#define SPINLOCK_DEFINE(name)
#define PRIVATE_SPINLOCK_DEFINE(name)
#define SPINLOCK_DECLARE(name)
#define spinlock_init(sl)
#define spinlock_lock(sl)
#define spinlock_unlock(sl)

#else

/* SMP */
#define SPINLOCK_DEFINE(name)	spinlock_t name;
#define PRIVATE_SPINLOCK_DEFINE(name)	PRIVATE SPINLOCK_DEFINE(name)
#define SPINLOCK_DECLARE(name)	extern SPINLOCK_DEFINE(name)
#define spinlock_init(sl) do { (sl)->val = 0; } while (0)

#if CONFIG_MAX_CPUS == 1
#define spinlock_lock(sl)
#define spinlock_unlock(sl)
#else
void arch_spinlock_lock(atomic_t * sl);
void arch_spinlock_unlock(atomic_t * sl);
int arch_fetch_and_inc(atomic_t *counter);
#define spinlock_lock(sl)	arch_spinlock_lock(&((sl)->val))
#define spinlock_unlock(sl)	arch_spinlock_unlock(&((sl)->val))
#endif


#endif /* CONFIG_SMP */

#define BKL_LOCK() \
	do { \
		ktzprofile_event(KTRACE_BKL_TRY); \
		big_kernel_lock.lock(); \
		ktzprofile_event(KTRACE_BKL_ACQUIRE); \
	} while (0)

#define BKL_UNLOCK() \
	do { \
		ktzprofile_event(KTRACE_BKL_RELEASE); \
		big_kernel_lock.unlock(); \
	} while (0)

#endif /* __SPINLOCK_H__ */

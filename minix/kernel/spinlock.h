#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

//#include "kernel/kernel.h"
#include <machine/archtypes.h>

#define SPINLOCK_MAX_STACK_DEPTH 16
typedef struct spinlock {
	atomic_t val;
	volatile int owner;
	u32_t acquired_count;
	u32_t lock_stack_trace[SPINLOCK_MAX_STACK_DEPTH];
	u32_t unlock_stack_trace[SPINLOCK_MAX_STACK_DEPTH];
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
#define spinlock_init(sl) \
	do { \
		(sl)->val = 0; \
		(sl)->owner = -1; \
		(sl)->acquired_count = 0; \
		(sl)->lock_stack_trace[0] = 0xffffffff; \
		(sl)->unlock_stack_trace[0] = 0xffffffff; \
	} while (0)

#if CONFIG_MAX_CPUS == 1
#define spinlock_lock(sl)
#define spinlock_unlock(sl)
#else
void arch_spinlock_lock(atomic_t * sl);
void arch_spinlock_unlock(atomic_t * sl);
#define spinlock_lock(sl)	arch_spinlock_lock(&((sl)->val))
#define spinlock_unlock(sl)	arch_spinlock_unlock(&((sl)->val))
#endif


#endif /* CONFIG_SMP */

/*#define BKL_LOCK()	spinlock_lock(&big_kernel_lock)
#define BKL_UNLOCK()	spinlock_unlock(&big_kernel_lock)*/

volatile int __gdb_lock_owner;
void __gdb_bkl_lock(spinlock_t *lock, int cpu);
void __gdb_bkl_unlock(spinlock_t *lock, int cpu);

#define BKL_LOCK() \
	do { \
		spinlock_lock(&big_kernel_lock); \
		__gdb_bkl_lock(&big_kernel_lock, cpuid); \
	} while (0)

#define BKL_UNLOCK() \
	do { \
		__gdb_bkl_unlock(&big_kernel_lock, cpuid); \
		spinlock_unlock(&big_kernel_lock); \
	} while (0)

#endif /* __SPINLOCK_H__ */

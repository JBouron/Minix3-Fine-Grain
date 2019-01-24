#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

//#include "kernel/kernel.h"
#include <machine/archtypes.h>
#include "ktzprofile.h"

#define SPINLOCK_MAX_STACK_DEPTH 16
typedef struct spinlock {
	atomic_t val;
	volatile int owner;
	u32_t acquired_count;
	u32_t lock_stack_trace[SPINLOCK_MAX_STACK_DEPTH];
	u32_t unlock_stack_trace[SPINLOCK_MAX_STACK_DEPTH];
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
#define reetrantlock_lock(rl)
#define reetrantlock_unlock(rl)
#else
void arch_spinlock_lock(atomic_t * sl);
void arch_spinlock_unlock(atomic_t * sl);
#define spinlock_lock(sl)	arch_spinlock_lock(&((sl)->val))
#define spinlock_unlock(sl)	arch_spinlock_unlock(&((sl)->val))

#define reetrantlock_lock(rl)	_reentrantlock_lock(&(rl));
#define reetrantlock_unlock(rl)	_reentrantlock_unlock(&(rl));
#endif


#endif /* CONFIG_SMP */

/*#define BKL_LOCK()	spinlock_lock(&big_kernel_lock)
#define BKL_UNLOCK()	spinlock_unlock(&big_kernel_lock)*/

volatile int __gdb_lock_owner;
void __gdb_bkl_lock(spinlock_t *lock, int cpu);
void __gdb_bkl_unlock(spinlock_t *lock, int cpu);

void lock_all_procs(void);
void unlock_all_procs(void);
/* Unlock all the procs of the proc table except for `except_proc_nr`. */
void unlock_all_procs_except(int except_proc_nr);

/* To lock/unlock the BKL from asm: */
void bkl_lock(void);
void bkl_unlock(void);
extern int bkl_line;
extern char *bkl_file;

#define BKL_LOCK() \
	do { \
		ktzprofile_event(KTRACE_BKL_TRY); \
		lock_all_procs(); \
		ktzprofile_event(KTRACE_BKL_ACQUIRE); \
		bkl_line = __LINE__; \
		bkl_file = __FILE__; \
		__gdb_bkl_lock(&big_kernel_lock, cpuid); \
	} while (0)

#define BKL_UNLOCK() \
	do { \
		__gdb_bkl_unlock(&big_kernel_lock, cpuid); \
		ktzprofile_event(KTRACE_BKL_RELEASE); \
		unlock_all_procs(); \
	} while (0)

#endif /* __SPINLOCK_H__ */

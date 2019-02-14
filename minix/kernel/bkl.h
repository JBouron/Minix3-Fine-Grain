#ifndef __BKL_H__
#define __BKL_H__

#define BKL_SPINLOCK

/* This is the definition of the big_kernel_lock.
 * The BKL must implement three functions : init, lock and unlock.
 * Abracting the BKL gives us some flexibility to experiment with different
 * locking algorithms.
 * The default implementation uses a spinlock.
 */

typedef struct bkl {
	void (*const init)(void);
	void (*const lock)(void);
	void (*const unlock)(void);
	int owner;
} bkl_t;

#ifdef BKL_MCS
struct mcs_node {
	volatile char			must_wait;
	volatile struct mcs_node	*next;
};
#endif

extern bkl_t big_kernel_lock;
#endif

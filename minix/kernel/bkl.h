#ifndef __BKL_H__
#define __BKL_H__

#include "spinlock.h"

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
} bkl_t;

extern bkl_t big_kernel_lock;
#endif

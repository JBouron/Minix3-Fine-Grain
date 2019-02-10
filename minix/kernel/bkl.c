#include "bkl.h"

/* Default implementation of the BKL: spinlock. */
spinlock_t bkl_spinlock_underlying_lock;
void bkl_spinlock_init(void)
{
	spinlock_init(&bkl_spinlock_underlying_lock);
}

void bkl_spinlock_lock(void)
{
	spinlock_lock(&bkl_spinlock_underlying_lock);
}

void bkl_spinlock_unlock(void)
{
	spinlock_unlock(&bkl_spinlock_underlying_lock);
}

/* The bkl to use. */
bkl_t big_kernel_lock = {
	.init = bkl_spinlock_init,
	.lock = bkl_spinlock_lock,
	.unlock = bkl_spinlock_unlock,
};

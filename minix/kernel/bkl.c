#include "bkl.h"

#define BKL_SPINLOCK

#if defined(BKL_SPINLOCK)
/* Spinlock implementation of the BKL. */
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

#elif defined(BKL_TICKETLOCK)
/* Ticket-lock implementation of the BKL.
 * WARN: We should use 64 bits for the tickets to avoid wrapping.
 */
typedef struct ticketlock {
	atomic_t	next_ticket;
	atomic_t	now_serving;
} ticketlock_t;
ticketlock_t bkl_ticketlock_underlying_lock;

void bkl_ticketlock_init(void)
{
	bkl_ticketlock_underlying_lock.next_ticket = 0;
	bkl_ticketlock_underlying_lock.now_serving = 0;
}

void bkl_ticketlock_lock(void)
{
	atomic_t *const counter = &bkl_ticketlock_underlying_lock.next_ticket;
	const int ticket = arch_fetch_and_inc(counter);
	while(ticket!=bkl_ticketlock_underlying_lock.now_serving);
}

void bkl_ticketlock_unlock(void)
{
	bkl_ticketlock_underlying_lock.now_serving++;
}

/* The bkl to use. */
bkl_t big_kernel_lock = {
	.init = bkl_ticketlock_init,
	.lock = bkl_ticketlock_lock,
	.unlock = bkl_ticketlock_unlock,
};

#else
#error No BKL implementation
#endif

#include "ticketlock.h"

void ticketlock_init(ticketlock_t *lock)
{
	lock->next_ticket = 0;
	lock->now_serving = 0;
}

void ticketlock_lock(ticketlock_t *lock)
{
	/* Fetch a ticket and wait for our turn. */
	volatile atomic_t *const counter = &(lock->next_ticket);
	const int ticket = arch_fetch_and_inc(counter);
	while(ticket!=lock->now_serving);
}

void ticketlock_unlock(ticketlock_t *lock)
{
	/* We can do an non atomic store here, as we are in a single writer
	 * scenario. */
	lock->now_serving++;
}

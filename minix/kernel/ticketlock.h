#ifndef TICKETLOCK_H
#define TICKETLOCK_H
#include "spinlock.h" /* For atomic operations */

typedef struct ticketlock {
	volatile atomic_t	next_ticket;
	volatile atomic_t	now_serving;
} ticketlock_t;

/*
 * Initialize a ticketlock before use.
 */
void ticketlock_init(ticketlock_t *lock);
/*
 * Lock and unlock function for the ticketlock.
 */
void ticketlock_lock(ticketlock_t *lock);
void ticketlock_unlock(ticketlock_t *lock);
#endif

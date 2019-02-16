#include "mcs.h"
#include "spinlock.h" // For atomic operations
#include <assert.h>

void mcslock_init(mcslock_t *lock)
{
	*lock = NULL;
}

void mcslock_lock(mcslock_t *lock,mcs_node_t *I)
{
	I->next = NULL;
	mcs_node_t *pred = fetch_and_store(lock,I);
	assert(pred!=I);
	if(pred!=NULL) {
		I->must_wait = 1;
		pred->next = I;
		while(I->must_wait);
	}
}

void mcslock_unlock(mcslock_t *lock,mcs_node_t *I)
{
	if(I->next==NULL) {
		const mcs_node_t *old = compare_and_swap(lock,I,NULL);
		if(old==I) {
			/* CAS succeeded, the lock is free. */
			return;
		} else {
			/* The CAS didn't succeed, another cpu will enqueue
			 * itself onto us. Just wait for it to set our `next`
			 * pointer. */
			while(I->next==NULL);
		}
	}
	I->next->must_wait = 0;
}

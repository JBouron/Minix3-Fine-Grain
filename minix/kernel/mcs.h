#ifndef MCS_H
#define MCS_H
typedef struct mcs_node {
	volatile char			must_wait;
	volatile struct mcs_node	*next;
} mcs_node_t;

typedef mcs_node_t* mcslock_t;

/*
 * Init an MCS lock.
 */
void mcslock_init(mcslock_t *lock);

/*
 * Lock and Unlock and MCS lock. The callee must provide a mcs_node_t that
 * will be used for the duration of the lock acquisition.
 */
void mcslock_lock(mcslock_t *lock,mcs_node_t *I);
void mcslock_unlock(mcslock_t *lock,mcs_node_t *I);
#endif

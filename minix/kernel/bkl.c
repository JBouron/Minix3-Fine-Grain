#include "bkl.h"
#include "smp.h"
#include <assert.h>
#include <string.h>

/* ============================================================================
 * 			SPINLOCK BKL
 * ==========================================================================*/
static spinlock_t bkl_spinlock_underlying_lock;

void bkl_spinlock_init(void)
{
	spinlock_init(&bkl_spinlock_underlying_lock);
}

void bkl_spinlock_lock(void)
{
	spinlock_lock(&bkl_spinlock_underlying_lock);
	assert(big_kernel_lock.owner==-1);
	big_kernel_lock.owner = cpuid;
}

void bkl_spinlock_unlock(void)
{
	assert(big_kernel_lock.owner==cpuid);
	big_kernel_lock.owner = -1;
	spinlock_unlock(&bkl_spinlock_underlying_lock);
}

/* ============================================================================
 * 			TICKETLOCK BKL
 * ==========================================================================*/

/* WARN: We should use 64 bits for the tickets to avoid wrapping. */
typedef struct ticketlock {
	atomic_t	next_ticket;
	atomic_t	now_serving;
} ticketlock_t;
static ticketlock_t bkl_ticketlock_underlying_lock;

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
	assert(big_kernel_lock.owner==-1);
	big_kernel_lock.owner = cpuid;
}

void bkl_ticketlock_unlock(void)
{
	assert(big_kernel_lock.owner==cpuid);
	big_kernel_lock.owner = -1;
	bkl_ticketlock_underlying_lock.now_serving++;
}

/* ============================================================================
 * 			MCS BKL
 * ==========================================================================*/
static struct mcs_node *bkl_mcs_underlying_lock;

void bkl_mcs_init(void)
{
	int i;
	bkl_mcs_underlying_lock = NULL;
	/* We are just booting, thus no race conditions here. */
	for(i=0;i<CONFIG_MAX_CPUS;++i) {
		struct mcs_node *const node = get_cpu_var_ptr(i,mcs_node);
		node->must_wait = 0;
		node->next = NULL;
	}
}

void bkl_mcs_lock(void)
{
	struct mcs_node *I = get_cpulocal_var_ptr(mcs_node);
	I->next = NULL;
	struct mcs_node *pred = fetch_and_store(&bkl_mcs_underlying_lock,I);
	assert(pred!=I);
	if(pred!=NULL) {
		I->must_wait = 1;
		pred->next = I;
		while(I->must_wait);
	}
	assert(big_kernel_lock.owner==-1);
	big_kernel_lock.owner = cpuid;
}

void bkl_mcs_unlock(void)
{
	assert(big_kernel_lock.owner==cpuid);
	big_kernel_lock.owner = -1;
	struct mcs_node *I = get_cpulocal_var_ptr(mcs_node);
	if(I->next==NULL) {
		const struct mcs_node *old =
			compare_and_swap(&bkl_mcs_underlying_lock,I,NULL);
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

bkl_t big_kernel_lock;
void create_bkl(const char *const name)
{
	if(!strcmp(name,"spinlock")) {
		big_kernel_lock = (bkl_t) {
			.init = bkl_spinlock_init,
			.lock = bkl_spinlock_lock,
			.unlock = bkl_spinlock_unlock,
		};
	} else if(!strcmp(name,"ticketlock")) {
		big_kernel_lock = (bkl_t) {
			.init = bkl_ticketlock_init,
			.lock = bkl_ticketlock_lock,
			.unlock = bkl_ticketlock_unlock,
		};
	} else if(!strcmp(name,"mcs")) {
		big_kernel_lock = (bkl_t) {
			.init = bkl_mcs_init,
			.lock = bkl_mcs_lock,
			.unlock = bkl_mcs_unlock,
		};
	} else {
		panic("Unknown BKL implementation name");
	}
	/* Reset owner field. */
	big_kernel_lock.owner = -1;
}

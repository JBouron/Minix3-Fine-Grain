#include "bkl.h"
#include "smp.h"
#include <assert.h>

#if defined(BKL_SPINLOCK)
/* Spinlock implementation of the BKL. */
spinlock_t bkl_spinlock_underlying_lock;

static u32_t lock_stacktrace[16];
static u32_t unlock_stacktrace[16];

static void fill_stack_trace(u32_t *dest, u32_t ebp, u32_t n) {
	int i;
	for(i = 0; i < n && ebp; ++i) {
		u32_t calling_eip = ((u32_t *)ebp)[1];
		dest[i] = calling_eip;
		ebp = ((u32_t *)ebp)[0];
	}
	if (!ebp)
		dest[i] = 0x0;
}

static void reset_stack_trace(u32_t *dest, u32_t n) {
	int i;
	for(i = 0; i < n; ++i)
		dest[i] = 0xffffffff;
}

void bkl_spinlock_init(void)
{
	spinlock_init(&bkl_spinlock_underlying_lock);
	big_kernel_lock.owner = -1;
	reset_stack_trace(lock_stacktrace,16);
	reset_stack_trace(unlock_stacktrace,16);
}

void bkl_spinlock_lock(void)
{
	spinlock_lock(&bkl_spinlock_underlying_lock);
	assert(big_kernel_lock.owner==-1);
	big_kernel_lock.owner = cpuid;
	fill_stack_trace(lock_stacktrace,get_stack_frame(),16);
	reset_stack_trace(unlock_stacktrace,16);
}

void bkl_spinlock_unlock(void)
{
	assert(big_kernel_lock.owner==cpuid);
	big_kernel_lock.owner = -1;
	fill_stack_trace(unlock_stacktrace,get_stack_frame(),16);
	reset_stack_trace(lock_stacktrace,16);
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
	big_kernel_lock.owner = -1;
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

/* The bkl to use. */
bkl_t big_kernel_lock = {
	.init = bkl_ticketlock_init,
	.lock = bkl_ticketlock_lock,
	.unlock = bkl_ticketlock_unlock,
};

#elif defined(BKL_MCS)
/* MCS-based BKL implementation. */
struct mcs_node *bkl_mcs_underlying_lock;

void bkl_mcs_init(void)
{
	int i;
	bkl_mcs_underlying_lock = NULL;
	big_kernel_lock.owner = -1;
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

/* The bkl to use. */
bkl_t big_kernel_lock = {
	.init = bkl_mcs_init,
	.lock = bkl_mcs_lock,
	.unlock = bkl_mcs_unlock,
};
#else
#error No BKL implementation
#endif

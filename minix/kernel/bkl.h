#ifndef __BKL_H__
#define __BKL_H__

/* This is the definition of the big_kernel_lock.
 * The BKL must implement three functions : init, lock and unlock.
 * Abracting the BKL gives us some flexibility to experiment with different
 * locking algorithms.
 * The default implementation uses a spinlock.
 */
#define BKL_DEFAULT_IMPL	"spinlock"

typedef struct bkl {
	void (*const init)(void);
	void (*const lock)(void);
	void (*const unlock)(void);
	int owner;
} bkl_t;

struct mcs_node {
	volatile char			must_wait;
	volatile struct mcs_node	*next;
};

/* Declare the BKL here. */
extern bkl_t big_kernel_lock;

/* This creates the BKL given the name.
 * `name` can be : "spinlock","ticketlock" or "mcs".
 */
void create_bkl(const char *const name);
#endif

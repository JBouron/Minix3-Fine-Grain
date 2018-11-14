#ifndef KTZPROFILE_H
#define KTZPROFILE_H

#include <sys/types.h>

/* Contains all the data, per cpu, used to profile the kernel. */
struct ktzprofile_data {
	/* Profile the time waiting for the BKL. */
	u64_t last_bkl_try_tsc;
	u64_t bkl_wait_tsc_delta_sum;
	u64_t bkl_wait_samples;
	u64_t bkl_wait_current_avg; /* In micro sec. */

	/* Profile the time holding the BKL. */
	u64_t last_bkl_acquire_tsc;
	u64_t critical_section_tsc_delta_sum;
	u64_t critical_section_samples;
	u64_t critical_section_current_avg; /* In micro sec. */
};

extern struct ktzprofile_data ktzprofile_per_cpu_data[CONFIG_MAX_CPUS];
extern unsigned ktzprofile_enabled;

/* Signal the profiler that we are trying to acquire BKL. */
void ktzprofile_try_bkl(void);
/* Signal the profiler that we acquired BKL. */
void ktzprofile_acquire_bkl(void);
/* Signal the profiler that we released BKL. */
void ktzprofile_release_bkl(void);

#endif

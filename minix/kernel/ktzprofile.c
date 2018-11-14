#include "ktzprofile.h"

/* Per-cpu profiling. */
struct ktzprofile_data ktzprofile_per_cpu_data[CONFIG_MAX_CPUS];
unsigned ktzprofile_enabled = 0;

/* Trick to avoid build errors. */
extern int __gdb_cpuid(void);
extern void read_tsc_64(u64_t*);
extern u64_t cpu_hz[CONFIG_MAX_CPUS]; 

#define KTZPROFILE_CHECK_ENABLED() do{if(!ktzprofile_enabled)return;}while(0)

static u64_t make64(unsigned long lo, unsigned long hi)
{
	return ((u64_t)hi << 32) | (u64_t)lo;
}

void ktzprofile_try_bkl(void)
{
	KTZPROFILE_CHECK_ENABLED();
	/* Simply remember when we started trying. */
	read_tsc_64(&(ktzprofile_per_cpu_data[__gdb_cpuid()].last_bkl_try_tsc));
}

void ktzprofile_acquire_bkl(void)
{
	/* Compute the time we waited for the lock and update the field in the
	 * cpu local data. */
	u64_t now,delta;
	struct ktzprofile_data *data;

	KTZPROFILE_CHECK_ENABLED();

	data = &ktzprofile_per_cpu_data[__gdb_cpuid()];
	read_tsc_64(&now);

	/* It might happen that we start the profiling between the TRY and
	 * ACQUIRE. In this case the last_bkl_try_tsc will not be set (eg.
	 * default value of 0). If it happens simply ignore this sample. */
	if(!data->last_bkl_try_tsc)
		return;

	/* Compute the delta cycles between the last try. */
	delta = now-data->last_bkl_try_tsc;
	data->bkl_wait_tsc_delta_sum += delta;
	data->bkl_wait_samples ++;

	/* Update the tsc at which we got the BKL. */
	data->last_bkl_acquire_tsc = now;
	data->last_bkl_try_tsc = 0;

	/* Update the current avg. */
	data->bkl_wait_current_avg =
		(make64(1000000,0)*data->bkl_wait_tsc_delta_sum/
		data->bkl_wait_samples)/cpu_hz[0];
}

void ktzprofile_release_bkl(void)
{
	/* Compute the time (in cycles) we spend in the critical section
	 * (using last_bkl_acquire_tsc), and update the running average of
	 * the critical section lenght. */
	u64_t now,delta;
	struct ktzprofile_data *data;

	KTZPROFILE_CHECK_ENABLED();

	data = &ktzprofile_per_cpu_data[__gdb_cpuid()];
	read_tsc_64(&now);

	/* It might happen that we start the profiling between the ACQUIRE and
	 * RELEASE. In this case the last_bkl_acquire_tsc will not be set (eg.
	 * default value of 0). If it happens simply ignore this sample. */
	if(!data->last_bkl_acquire_tsc)
		return;

	/* Compute the delta cycles between the time we acquired the BKL and
	 * now. */
	delta = now-data->last_bkl_acquire_tsc;
	data->critical_section_tsc_delta_sum += delta;
	data->critical_section_samples ++;

	data->last_bkl_acquire_tsc = 0;

	/* Update the current avg. */
	data->critical_section_current_avg =
		(make64(1000000,0)*data->critical_section_tsc_delta_sum/
		data->critical_section_samples)/cpu_hz[0];
}

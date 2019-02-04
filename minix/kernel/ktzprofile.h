#ifndef KTZPROFILE_H
#define KTZPROFILE_H

#include "ktrace.h"
#include <sys/types.h>
#include "minix/ipc.h"

/* Keep statistics of the time spent between two events A and B.
 * We keep the average and the total time spent.
 */
struct ktzprofile_stat {
	int event_a;		/* The event triggering the measure. */
	int event_b;		/* The event marking the end of the measure. */

	u64_t last_event_a_tsc;	/* Last TSC we encountered A. */
	u64_t delta_sum;	/* The sum of all the cycles delta [A,B]. */
	u64_t samples;		/* The number of measures. */

	/* The stats under this line are really what the user is looking for. */
	u64_t delta_avg_usec;	/* The last computed average time(us) [A,B]. */
	u64_t tot_time_usec;	/* The sum of all time(us) spent in [A,B]. */
	u64_t min_delta_usec;	/* The minimum time(us) spent in [A,B]. */
	u64_t max_delta_usec;	/* The maximum time(us) spent in [A,B]. */
};

/* From com.h */
#define KTZPROFILE_MSG_LOW 0x0
#define KTZPROFILE_MSG_HIGH 0x1AFF
#define KTZPROFILE_MSG_BIN_SIZE 0x100
#define KTZPROFILE_MSG_RANGE_HIST_SIZE ((KTZPROFILE_MSG_HIGH+1)/KTZPROFILE_MSG_BIN_SIZE)
struct ktzprofile_msg_range_hist {
	/* The bins. */
	int bins[KTZPROFILE_MSG_RANGE_HIST_SIZE];
	/* Number of invalid message type witnessed. */
	int reserved;
};

#define KTZPROFILE_MSG_HIST_SIZE ((KTZPROFILE_MSG_HIGH+1))
struct ktzprofile_msg_hist {
	/* The bins (one per message type). */
	int bins[KTZPROFILE_MSG_HIST_SIZE];
	/* Number of invalid message type witnessed. */
	int reserved;
};

/* Contains all the data, per cpu, used to profile the kernel. */
struct ktzprofile_data {
	/* TCS during the very first and last samples respectively. */
	u64_t first_sample_tsc;
	u64_t last_sample_tsc;

	/* Stats on the time spent waiting for the BKL. */
	struct ktzprofile_stat bkl_stats;
	/* Stats on the time spent in the kernel/critical section. */
	struct ktzprofile_stat critical_section_stats;
	/* Stats for the idling time. */
	struct ktzprofile_stat idle_time_stats;
	/* Stats for time spent in user space. */
	struct ktzprofile_stat userspace_time_stats;

	/* Stats for each kernel_call. */
	struct ktzprofile_stat kernel_call_stats[KTRACE_NUM_KERNEL_CALLS];

	/* Stats for each IPC. */
	struct ktzprofile_stat ipc_stats[KTRACE_NUM_IPCS];

	/* Histogram of each message type. */
	struct ktzprofile_msg_hist msg_hist;
	struct ktzprofile_msg_range_hist msg_range_hist;
};

extern struct ktzprofile_data ktzprofile_per_cpu_data[CONFIG_MAX_CPUS];
extern unsigned ktzprofile_enabled;

/* Init the stats for each cpu. */
void ktzprofile_init(void);
/* Tell the profiler of a new event. Update all the stats of this cpu. */
void ktzprofile_event(int ktrace_event);
/* Make the profiler aware of a kernel call, Note: call_nr is the "real"
 * call number, that is SYS_* and not KTRACE_*. */
void ktzprofile_kernel_call(int call_nr);
/* Make the profiler aware of an ipc, Note: call_nr is the "real"
 * call number, that is not KTRACE_*. */
void ktzprofile_ipc(int call_nr);
/* Make the profiler aware of a message type being sent.
 * This is a non timing related stat. */
void ktzprofile_deliver_msg(message *msg);
#endif

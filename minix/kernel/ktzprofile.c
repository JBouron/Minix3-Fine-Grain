#include "ktzprofile.h"
#include "kernel/smp.h"
#include "include/minix/ipcconst.h" /* For IPC codes. */
#include "include/minix/sysutil.h" /* For panic(). */
#include "include/minix/com.h" /* For kernel call codes. */

/* Per-cpu profiling. */
struct ktzprofile_data ktzprofile_per_cpu_data[CONFIG_MAX_CPUS];
unsigned ktzprofile_enabled = 0;

extern void read_tsc_64(u64_t*);
extern u64_t cpu_hz[CONFIG_MAX_CPUS]; 

#define KTZPROFILE_CHECK_ENABLED() do{if(!ktzprofile_enabled)return;}while(0)
#define min(a,b) ((a)>(b)?(b):(a))
#define max(a,b) ((a)>(b)?(a):(b))

static inline u64_t cycles_to_usec(u64_t cycles)
{
	u64_t usec;
	usec = make64(1000000,0);
	return (usec*cycles)/cpu_hz[cpuid];
}

static void init_stat(struct ktzprofile_stat *stat,int event_a,int event_b)
{
	stat->event_a = event_a;
	stat->event_b = event_b;
	stat->last_event_a_tsc = 0;
	stat->delta_sum = 0;
	stat->samples = 0;

	stat->delta_avg_usec = 0;
	stat->tot_time_usec = 0;
	stat->min_delta_usec = make64(0xffffffff,0xffffffff);
	stat->max_delta_usec = 0;
}

void ktzprofile_init(void)
{
	int cpu,i,event;
	struct ktzprofile_data *data;
	struct ktzprofile_stat *stat;

	for(cpu=0;cpu<CONFIG_MAX_CPUS;++cpu) {
		data = &ktzprofile_per_cpu_data[cpu];
		
		data->first_sample_tsc = 0;
		data->last_sample_tsc = 0;
		/* Init BKL and Critical section stats manually. */
		init_stat(&(data->bkl_stats),KTRACE_BKL_TRY,KTRACE_BKL_ACQUIRE);
		init_stat(&(data->critical_section_stats),
			  KTRACE_BKL_ACQUIRE,
			  KTRACE_BKL_RELEASE);
		init_stat(&(data->idle_time_stats),
			  KTRACE_IDLE_START,
			  KTRACE_IDLE_STOP);
		init_stat(&(data->userspace_time_stats),
			  KTRACE_USER_START,
			  KTRACE_USER_STOP);

		for(i=0;i<KTRACE_NUM_KERNEL_CALLS;++i) {
			event=KTRACE_SYS_FORK;
			for(;event<=KTRACE_SYS_PADCONF;++event) {
				stat = &(data->kernel_call_stats[event]);
				init_stat(stat,event,KTRACE_KERNEL_CALL_END);
			}
		}

		for(i=0;i<KTRACE_NUM_IPCS;++i) {
			event=KTRACE_SEND;
			for(;event<=KTRACE_SENDA;++event) {
				int e = event-KTRACE_SEND;
				stat = &(data->ipc_stats[e]);
				init_stat(stat,event,KTRACE_IPC_END);
			}
		}
	}
}

static void update_stat(struct ktzprofile_stat *stat,u64_t now,int ktrace_event)
{
	u64_t delta,delta_usec;
	if(ktrace_event==stat->event_a) {
		/* Register the start time. */
		stat->last_event_a_tsc = now;
	} else if(ktrace_event==stat->event_b) {
		/* If we started the profiling in a middle of [A,B] then
		 * ignore this sample. */
		if(!stat->last_event_a_tsc)
			return;

		/* Compute the delta cycles between the last event a. And
		 * update the running sum and the number of samples. */
		delta = now-stat->last_event_a_tsc;
		stat->delta_sum += delta;
		stat->samples ++;

		/* Update the last avg and total spent time between A and B. */
		stat->delta_avg_usec = cycles_to_usec(stat->delta_sum/stat->samples);
		stat->tot_time_usec = cycles_to_usec(stat->delta_sum);

		/* Update min and max. */
		delta_usec = cycles_to_usec(delta);
		stat->min_delta_usec = min(stat->min_delta_usec,delta_usec);
		stat->max_delta_usec = max(stat->max_delta_usec,delta_usec);

		stat->last_event_a_tsc = 0;
	} else {
		/* This stat does not concern this event, simply ignore it. */
	}
}

void ktzprofile_event(int ktrace_event)
{
	int i;
	struct ktzprofile_data *data;
	u64_t now;

	KTZPROFILE_CHECK_ENABLED();
	data = &ktzprofile_per_cpu_data[cpuid];
	read_tsc_64(&now);

	if(!data->first_sample_tsc)
		data->first_sample_tsc = now;
	data->last_sample_tsc = now;

	update_stat(&(data->bkl_stats),now,ktrace_event);
	update_stat(&(data->critical_section_stats),now,ktrace_event);
	update_stat(&(data->idle_time_stats),now,ktrace_event);
	update_stat(&(data->userspace_time_stats),now,ktrace_event);

	for(i=0;i<KTRACE_NUM_KERNEL_CALLS;++i)
		update_stat(&(data->kernel_call_stats[i]),now,ktrace_event);

	for(i=0;i<KTRACE_NUM_IPCS;++i)
		update_stat(&(data->ipc_stats[i]),now,ktrace_event);
}

void ktzprofile_kernel_call(int call_nr)
{
	/* Translate the kernel call first. */
	int translated;
	call_nr += KERNEL_CALL;
	if(call_nr<=SYS_SIGRETURN)
		translated = call_nr;
	else if(call_nr<=SYS_IRQCTL)
		translated = call_nr-2;
	else if(call_nr<=SYS_IOPENABLE)
		translated = call_nr-3;
	else if(call_nr<=SYS_SPROF)
		translated = call_nr-5;
	else if(call_nr<=SYS_SETTIME)
		translated = call_nr-7;
	else if(call_nr<=SYS_RUNCTL)
		translated = call_nr-9;
	else if(call_nr<=SYS_PADCONF)
		translated = call_nr-12;
	else
		return;
	translated -= KERNEL_CALL;
	if(!KTRACE_IS_KERNEL_CALL(translated))
		return;
	ktzprofile_event(translated);
}

void ktzprofile_ipc(int call_nr)
{
	int translated;
	if(call_nr==SENDA)
		translated = KTRACE_SENDA;
	else
		translated = KTRACE_EVENT_LOW+46-1+call_nr;
	if(!KTRACE_IS_IPC(translated))
		panic("Not an IPC event.");
	ktzprofile_event(translated);
}

void ktzprofile_deliver_msg(message *msg) {
	int bin_idx, type;
	endpoint_t src;
	struct ktzprofile_data *data;

	KTZPROFILE_CHECK_ENABLED();
	type = msg->m_type;
	src = msg->m_source;
	data = &ktzprofile_per_cpu_data[cpuid];

	/* Update msg hist. */
	bin_idx = type;
	if(bin_idx<=0||bin_idx>=KTZPROFILE_MSG_HIST_SIZE) {
		/* Message of type 0 are reserved. */
		data->msg_hist.reserved++;
	} else {
		data->msg_hist.bins[bin_idx]++;
	}

	/* Update range hist. */
	bin_idx = type/KTZPROFILE_MSG_BIN_SIZE;
	if(type==0||bin_idx<0||bin_idx>=KTZPROFILE_MSG_RANGE_HIST_SIZE) {
		/* Message of type 0 are reserved. */
		data->msg_range_hist.reserved++;
	} else {
		data->msg_range_hist.bins[bin_idx]++;
	}
}

#include <assert.h>

#include "smp.h"
#include "interrupt.h"
#include "clock.h"
#include "spinlock.h"

unsigned ncpus;
unsigned ht_per_core;
unsigned bsp_cpu_id;

struct cpu cpus[CONFIG_MAX_CPUS];

/* info passed to another cpu along with a sched ipi */
struct sched_ipi_data {
	/* Lock should be acquired when setting or clearing flag/data. */
	spinlock_t	lock;
	volatile u32_t	flags;
	volatile u32_t	data;
};

static struct sched_ipi_data  sched_ipi_data[CONFIG_MAX_CPUS];

#define SCHED_IPI_STOP_PROC	1
#define SCHED_IPI_VM_INHIBIT	2
#define SCHED_IPI_SAVE_CTX	4
#define SCHED_IPI_DEQUEUE	8
#define SCHED_IPI_MIGRATE	16

static volatile unsigned ap_cpus_booted;

SPINLOCK_DEFINE(vmrequest_lock)
SPINLOCK_DEFINE(big_kernel_lock)
SPINLOCK_DEFINE(boot_lock)

void wait_for_APs_to_finish_booting(void)
{
	unsigned n = 0;
	int i;

	/* check how many cpus are actually alive */
	for (i = 0 ; i < ncpus ; i++) {
		if (cpu_test_flag(i, CPU_IS_READY))
			n++;
	}
	if (n != ncpus)
		printf("WARNING only %d out of %d cpus booted\n", n, ncpus);

	/* we must let the other CPUs to run in kernel mode first */
	BKL_UNLOCK();
	while (ap_cpus_booted != (n - 1))
		arch_pause();
	/* now we have to take the lock again as we continue execution */
	BKL_LOCK();
}

void ap_boot_finished(unsigned cpu)
{
	ap_cpus_booted++;
}

void smp_ipi_halt_handler(void)
{
	ipi_ack();
	stop_local_timer();
	arch_smp_halt_cpu();
}

void smp_schedule(unsigned cpu)
{
	arch_send_smp_schedule_ipi(cpu,0);
}

void smp_sched_handler(void);

/*
 * tell another cpu about a task to do and return only after the cpu acks that
 * the task is finished. Also wait before it finishes task sent by another cpu
 * to the same one.
 */
static void smp_schedule_sync(struct proc * p, unsigned task)
{
	unsigned cpu = p->p_cpu;
	unsigned mycpu = cpuid;

	assert(cpu != mycpu);
	/*
	 * if some other cpu made a request to the same cpu, wait until it is
	 * done before proceeding
	 */
retry:
	if (sched_ipi_data[cpu].flags != 0) {
		while (sched_ipi_data[cpu].flags != 0) {
			if (sched_ipi_data[mycpu].flags) {
				smp_sched_handler();
			}
		}
	}

	/* We may have a chance ! */
	spinlock_lock(&(sched_ipi_data[cpu].lock));

	if (sched_ipi_data[cpu].flags != 0) {
		/* No luck, try again. */
		spinlock_unlock(&(sched_ipi_data[cpu].lock));
		goto retry;
	} else {
		/* We got lucky. Set the data and flag. */
		sched_ipi_data[cpu].data = (u32_t) p;
		sched_ipi_data[cpu].flags |= task;
	}

	spinlock_unlock(&(sched_ipi_data[cpu].lock));

	__insn_barrier();

	/* We are using NMIs only so that we can keep the BKL while the target
	 * cpu is completing the request (which doesn't need the BKL).
	 * Because we don't release and re-acquire the BKL we don't violate
	 * the lock ordering wrt to the proc lock(s) we may have. */
	int nmi = 1;
	arch_send_smp_schedule_ipi(cpu,nmi);

	/* wait until the destination cpu finishes its job */
	while (sched_ipi_data[cpu].flags != 0) {
		if (sched_ipi_data[mycpu].flags) {
			smp_sched_handler();
		}
	}
}

void smp_schedule_stop_proc(struct proc * p)
{
	if (proc_is_runnable(p))
		smp_schedule_sync(p, SCHED_IPI_STOP_PROC);
	else
		RTS_SET(p, RTS_PROC_STOP);
	assert(RTS_ISSET(p, RTS_PROC_STOP));
}

void smp_schedule_vminhibit(struct proc * p)
{
	if (proc_is_runnable(p))
		smp_schedule_sync(p, SCHED_IPI_VM_INHIBIT);
	else
		RTS_SET(p, RTS_VMINHIBIT);
	assert(RTS_ISSET(p, RTS_VMINHIBIT));
}

void smp_schedule_stop_proc_save_ctx(struct proc * p)
{
	/*
	 * stop the processes and force the complete context of the process to
	 * be saved (i.e. including FPU state and such)
	 */
	smp_schedule_sync(p, SCHED_IPI_STOP_PROC | SCHED_IPI_SAVE_CTX);
	assert(RTS_ISSET(p, RTS_PROC_STOP));
}

void smp_schedule_migrate_proc(struct proc * p, unsigned dest_cpu)
{
	/*
	 * stop the processes and force the complete context of the process to
	 * be saved (i.e. including FPU state and such)
	 */
	assert(proc_locked(p));
	/* The proc should not be in a middle of a migration already. */
	assert(!(p->p_rts_flags&RTS_PROC_MIGR));
	assert(p->p_cpu!=cpuid);
	assert(p->p_cpu!=dest_cpu);
	p->p_next_cpu = dest_cpu;
	smp_schedule_sync(p,SCHED_IPI_MIGRATE);
}

void smp_dequeue_task(struct proc *p)
{
	assert(p->p_enqueued);
	assert(proc_locked(p));
	smp_schedule_sync(p,SCHED_IPI_DEQUEUE);
}

void smp_sched_handler(void)
{
	unsigned flgs;
	unsigned cpu = cpuid;

	/* TODO: remove lock/unlock here. */
	spinlock_lock(&(sched_ipi_data[cpu].lock));
	flgs = sched_ipi_data[cpu].flags;
	spinlock_unlock(&(sched_ipi_data[cpu].lock));

	if (flgs) {
		struct proc * p;
		p = (struct proc *)sched_ipi_data[cpu].data;

		/* The cpu triggering this NMI must always have the lock on the
		 * proc. */
		assert(proc_locked_borrow(p));

		if (flgs & SCHED_IPI_STOP_PROC) {
			RTS_SET_BORROW(p, RTS_PROC_STOP);
		}
		if (flgs & SCHED_IPI_SAVE_CTX) {
			/* all context has been saved already, FPU remains */
			if (proc_used_fpu(p) &&
					get_cpulocal_var(fpu_owner) == p) {
				disable_fpu_exception();
				save_local_fpu(p, FALSE /*retain*/);
				/* we're preparing to migrate somewhere else */
				release_fpu(p);
			}
		}
		if (flgs & SCHED_IPI_VM_INHIBIT) {
			RTS_SET_BORROW(p, RTS_VMINHIBIT);
		}
		if (flgs&SCHED_IPI_DEQUEUE) {
			assert(p->p_cpu==cpuid);
			dequeue(p);
		}
		if (flgs&SCHED_IPI_MIGRATE) {
			assert(p->p_cpu==cpu);
			if(get_cpu_var(cpu,proc_ptr)==p) {
				/* This proc might be in the middle of it's
				 * user timeslice or in a kernel operation.
				 * Thus let it finish and defer the migration
				 * until the switch_to_user by setting the
				 * RTS_PROC_MIGR flag. */
				assert(p->p_next_cpu!=cpu);
				assert(p->p_next_cpu!=-1);
				RTS_SET_BORROW(p,RTS_PROC_MIGR);
			} else {
				/* This proc is either not runnable, or
				 * waiting in this cpu's ready queue.
				 * Either way it is not currently running
				 * and thus we can safely migrate it now. */
				RTS_SET_BORROW(p,RTS_PROC_MIGR);
				p->p_cpu = p->p_next_cpu;
				p->p_next_cpu = -1;
				RTS_UNSET_BORROW(p,RTS_PROC_MIGR);
			}
		}
	}

	__insn_barrier();
	spinlock_lock(&(sched_ipi_data[cpu].lock));
	sched_ipi_data[cpu].flags = 0;
	spinlock_unlock(&(sched_ipi_data[cpu].lock));
}

/*
 * This function gets always called only after smp_sched_handler() has been
 * already called. It only serves the purpose of acknowledging the IPI and
 * preempting the current process if the CPU was not idle.
 */
void smp_ipi_sched_handler(void)
{
	struct proc * curr;

	ipi_ack();

	/* We used to preempt the curr here, no matter what. However a race
	 * condition can (and will arise) if the remote core is picking the
	 * next task to run at the same time. In this case the IPI will mark
	 * this task as RTS_PREEMPTED and the assert(proc_is_runnable(p)) in
	 * switch_to_user will fail.
	 */
#if 0
	curr = get_cpulocal_var(proc_ptr);
	if (curr->p_endpoint != IDLE) {
		RTS_SET(curr, RTS_PREEMPTED);
	}
#endif
}


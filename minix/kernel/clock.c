/* This file contains the architecture-independent clock functionality, which
 * handles time related functions.  Important events that are handled here
 * include setting and monitoring alarm timers and deciding when to
 * (re)schedule processes.  System services can access its services through
 * system calls, such as sys_setalarm().
 *
 * Changes:
 *   Aug 18, 2006   removed direct hardware access etc, MinixPPC (Ingmar Alting)
 *   Oct 08, 2005   reordering and comment editing (A. S. Woodhull)
 *   Mar 18, 2004   clock interface moved to SYSTEM task (Jorrit N. Herder)
 *   Sep 30, 2004   source code documentation updated  (Jorrit N. Herder)
 *   Sep 24, 2004   redesigned alarm timers  (Jorrit N. Herder)
 */

#include <minix/endpoint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "clock.h"

#ifdef USE_WATCHDOG
#include "watchdog.h"
#endif

/* Function prototype for PRIVATE functions.
 */
static void load_update(void);

/* The CLOCK's timers queue. The functions in <minix/timers.h> operate on this.
 * Each system process possesses a single synchronous alarm timer. If other
 * kernel parts want to use additional timers, they must declare their own
 * persistent (static) timer structure, which can be passed to the clock
 * via (re)set_kernel_timer().
 * When a timer expires its watchdog function is run by the CLOCK task.
 */
static minix_timer_t *clock_timers;	/* queue of CLOCK timers */
static spinlock_t clock_timers_lock;	/* To protect the queue of timers. */
#define lock_clock_timers() spinlock_lock(&(clock_timers_lock))
#define unlock_clock_timers() spinlock_unlock(&(clock_timers_lock))

/* Number of ticks to adjust realtime by. A negative value implies slowing
 * down realtime, a positive value implies speeding it up.
 */
static int32_t adjtime_delta = 0;

/*
 * Initialize the clock variables.
 */
void
init_clock(void)
{
	char *value;

	/* Initialize clock information structure. */
	memset(&kclockinfo, 0, sizeof(kclockinfo));

	/* Get clock tick frequency. */
	value = env_get("hz");
	if (value != NULL)
		kclockinfo.hz = atoi(value);
	if (value == NULL || kclockinfo.hz < 2 || kclockinfo.hz > 50000)
		kclockinfo.hz = DEFAULT_HZ;

	/* Load average data initialization. */
	memset(&kloadinfo, 0, sizeof(kloadinfo));
}

static void clock_timers_exp(clock_t now)
{
	/* Handle the expiration of clock_timers. Release the clock_timers_lock
	 */
	int nexp = 0;	/* How many expirations should we save ? */
	minix_timer_t *tp = clock_timers;

	while ( tp!=NULL && tmr_has_expired(tp, now)) {
		nexp ++;
		tp = tp->tmr_next;
	}

	int idx = 0;
	endpoint_t exps[nexp]; /* All the endpoint that need to recieve the
				  signal from CLOCK. */

	minix_timer_t **t = &clock_timers;
	while ((tp = *t) != NULL && tmr_has_expired(tp, now)) {
		*t = tp->tmr_next;
		assert(tp->tmr_func==cause_alarm);
		tp->tmr_func = NULL;
		exps[idx++] = tp->tmr_arg;
	}

	/* We now have all the endpoints that need a notify, release the timer
	 * lock and send all the notifs. */
	unlock_clock_timers();

	int i;
	for(i=0;i<nexp;++i) {
		cause_alarm(exps[i]);
	}
}

/*
 * The boot processor's timer interrupt handler. In addition to non-boot cpus
 * it keeps real time and notifies the clock task if need be.
 */
int timer_int_handler(void)
{
	/* Update user and system accounting times. Charge the current process
	 * for user time. If the current process is not billable, that is, if a
	 * non-user process is running, charge the billable process for system
	 * time as well.  Thus the unbillable process' user time is the billable
	 * user's system time.
	 */

	struct proc * p, * billp;

	/* FIXME watchdog for slave cpus! */
#ifdef USE_WATCHDOG
	/*
	 * we need to know whether local timer ticks are happening or whether
	 * the kernel is locked up. We don't care about overflows as we only
	 * need to know that it's still ticking or not
	 */
	watchdog_local_timer_ticks++;
#endif

	if (cpu_is_bsp(cpuid)) {
		kclockinfo.uptime++;

		/* if adjtime_delta has ticks remaining, apply one to realtime.
		 * limit changes to every other interrupt.
		 */
		if (adjtime_delta != 0 && kclockinfo.uptime & 0x1) {
			/* go forward or stay behind */
			kclockinfo.realtime += (adjtime_delta > 0) ? 2 : 0;
			adjtime_delta += (adjtime_delta > 0) ? -1 : +1;
		} else {
			kclockinfo.realtime++;
		}
	}

	/* Update user and system accounting times. Charge the current process
	 * for user time. If the current process is not billable, that is, if a
	 * non-user process is running, charge the billable process for system
	 * time as well.  Thus the unbillable process' user time is the billable
	 * user's system time.
	 */

	p = get_cpulocal_var(proc_ptr);
	billp = get_cpulocal_var(bill_ptr);

	lock_two_procs(p,billp);

	p->p_user_time++;

	if (! (priv(p)->s_flags & BILLABLE)) {
		billp->p_sys_time++;
	}

	/* Decrement virtual timers, if applicable. We decrement both the
	 * virtual and the profile timer of the current process, and if the
	 * current process is not billable, the timer of the billed process as
	 * well.  If any of the timers expire, do_clocktick() will send out
	 * signals.
	 */
	if ((p->p_misc_flags & MF_VIRT_TIMER) && (p->p_virt_left > 0)) {
		p->p_virt_left--;
	}
	if ((p->p_misc_flags & MF_PROF_TIMER) && (p->p_prof_left > 0)) {
		p->p_prof_left--;
	}
	if (! (priv(p)->s_flags & BILLABLE) &&
			(billp->p_misc_flags & MF_PROF_TIMER) &&
			(billp->p_prof_left > 0)) {
		billp->p_prof_left--;
	}

	/*
	 * Check if a process-virtual timer expired. Check current process, but
	 * also bill_ptr - one process's user time is another's system time, and
	 * the profile timer decreases for both!
	 */
	vtimer_check(p);

	if (p != billp)
		vtimer_check(billp);

	unlock_two_procs(p,billp);

	/* Update load average. */
	load_update();

	if (cpu_is_bsp(cpuid)) {
		/*
		 * If a timer expired, notify the clock task.  Keep in mind
		 * that clock tick values may overflow, so we must only look at
		 * relative differences, and only if there are timers at all.
		 */
		lock_clock_timers();
		if (clock_timers != NULL &&
		    tmr_has_expired(clock_timers, kclockinfo.uptime)) {
			clock_timers_exp(kclockinfo.uptime);
			/* The timer lock is released by clock_timers_exp. */
		} else {
			unlock_clock_timers();
		}

#ifdef DEBUG_SERIAL
		if (kinfo.do_serial_debug)
			do_ser_debug();
#endif

	}

	arch_timer_int_handler();	/* No-op in i386. */

	return(1);					/* reenable interrupts */
}

/*===========================================================================*
 *				get_realtime				     *
 *===========================================================================*/
clock_t get_realtime(void)
{
  /* Get and return the current wall time in ticks since boot. */
  return(kclockinfo.realtime);
}

/*===========================================================================*
 *				set_realtime				     *
 *===========================================================================*/
void set_realtime(clock_t newrealtime)
{
  kclockinfo.realtime = newrealtime;
}

/*===========================================================================*
 *				set_adjtime_delta			     *
 *===========================================================================*/
void set_adjtime_delta(int32_t ticks)
{
  adjtime_delta = ticks;
}

/*===========================================================================*
 *				get_monotonic				     *
 *===========================================================================*/
clock_t get_monotonic(void)
{
  /* Get and return the number of ticks since boot. */
  return(kclockinfo.uptime);
}

/*===========================================================================*
 *				set_boottime				     *
 *===========================================================================*/
void set_boottime(time_t newboottime)
{
  kclockinfo.boottime = newboottime;
}

/*===========================================================================*
 *				get_boottime				     *
 *===========================================================================*/
time_t get_boottime(void)
{
  /* Get and return the number of seconds since the UNIX epoch. */
  return(kclockinfo.boottime);
}

/*===========================================================================*
 *				set_kernel_timer			     *
 *===========================================================================*/
void set_kernel_timer(
  minix_timer_t *tp,			/* pointer to timer structure */
  clock_t exp_time,			/* expiration monotonic time */
  tmr_func_t watchdog,			/* watchdog to be called */
  int arg				/* argument for watchdog function */
)
{
/* Insert the new timer in the active timers list. Always update the
 * next timeout time by setting it to the front of the active list.
 */
  lock_clock_timers();
  (void)tmrs_settimer(&clock_timers, tp, exp_time, watchdog, arg, NULL, NULL);
  unlock_clock_timers();
}

/*===========================================================================*
 *				reset_kernel_timer			     *
 *===========================================================================*/
void reset_kernel_timer(
  minix_timer_t *tp			/* pointer to timer structure */
)
{
/* The timer pointed to by 'tp' is no longer needed. Remove it from both the
 * active and expired lists. Always update the next timeout time by setting
 * it to the front of the active list.
 */
  if (tmr_is_set(tp)) {
	lock_clock_timers();
	(void)tmrs_clrtimer(&clock_timers, tp, NULL, NULL);
	unlock_clock_timers();
  }
}

/*===========================================================================*
 *				load_update				     *
 *===========================================================================*/
static void load_update(void)
{
	u16_t slot;
	int enqueued = 0, q;
	struct proc *p;
	struct proc **rdy_head;

	/* Load average data is stored as a list of numbers in a circular
	 * buffer. Each slot accumulates _LOAD_UNIT_SECS of samples of
	 * the number of runnable processes. Computations can then
	 * be made of the load average over variable periods, in the
	 * user library (see getloadavg(3)).
	 */
	slot = (kclockinfo.uptime / system_hz / _LOAD_UNIT_SECS) %
	    _LOAD_HISTORY;
	if(slot != kloadinfo.proc_last_slot) {
		kloadinfo.proc_load_history[slot] = 0;
		kloadinfo.proc_last_slot = slot;
	}

	rdy_head = get_cpulocal_var(run_q_head);
	/* Cumulation. How many processes are ready now? */
	for(q = 0; q < NR_SCHED_QUEUES; q++) {
		for(p = rdy_head[q]; p != NULL; p = p->p_nextready) {
			enqueued++;
		}
	}

	kloadinfo.proc_load_history[slot] += enqueued;

	/* Up-to-dateness. */
	kloadinfo.last_clock = kclockinfo.uptime;
}

int boot_cpu_init_timer(unsigned freq)
{
	if (init_local_timer(freq))
		return -1;

	if (register_local_timer_handler(
				(irq_handler_t) timer_int_handler))
		return -1;

	return 0;
}

int app_cpu_init_timer(unsigned freq)
{
	if (init_local_timer(freq))
		return -1;

	return 0;
}

/*===========================================================================*
 *				cause_alarm				     *
 *===========================================================================*/
void cause_alarm(int proc_nr_e)
{
/* Routine called if a timer goes off and the process requested a synchronous
 * alarm. The process number is stored as the timer argument. Notify that
 * process with a notification message from CLOCK.
 */

  mini_notify(proc_addr(CLOCK), proc_nr_e);	/* notify process */
}


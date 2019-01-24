#include "kernel/system.h"
#include <minix/endpoint.h>
#include "kernel/clock.h"

/*===========================================================================*
 *				do_schedule				     *
 *===========================================================================*/
int do_schedule(struct proc * caller, message * m_ptr)
{
	struct proc *p;
	int proc_nr;
	int priority, quantum, cpu, niced;
	int res;

	if (!isokendpt(m_ptr->m_lsys_krn_schedule.endpoint, &proc_nr))
		return EINVAL;

	p = proc_addr(proc_nr);
	lock_proc(p);

	/* Only this process' scheduler can schedule it */
	if (caller != p->p_scheduler) {
		res = (EPERM);
	} else {
		/* Try to schedule the process. */
		priority = m_ptr->m_lsys_krn_schedule.priority;
		quantum = m_ptr->m_lsys_krn_schedule.quantum;
		cpu = m_ptr->m_lsys_krn_schedule.cpu;
		niced = !!(m_ptr->m_lsys_krn_schedule.niced);

		res = sched_proc(p, priority, quantum, cpu, niced);
		if(res) {
			printf("Error sched %s on %d\n", p->p_name, cpu);
		}
	}	
	unlock_proc(p);

	lock_proc(caller);
	return res;
}

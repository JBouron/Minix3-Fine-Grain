#include "kernel/system.h"
#include <minix/endpoint.h>

/*===========================================================================*
 *			          do_schedctl			     *
 *===========================================================================*/
static int do_schedctl_impl(struct proc * caller, struct proc *p, message * m_ptr)
{
	int priority, quantum, cpu;
	int r;
	const uint32_t flags = m_ptr->m_lsys_krn_schedctl.flags;

	if ((flags & SCHEDCTL_FLAG_KERNEL) == SCHEDCTL_FLAG_KERNEL) {
		/* the kernel becomes the scheduler and starts 
		 * scheduling the process.
		 */
		static int dest_cpu = 0;
		priority = m_ptr->m_lsys_krn_schedctl.priority;
		quantum = m_ptr->m_lsys_krn_schedctl.quantum;

		/* Dirty trick: We spread the system processes on all the cpus.
		 */
		cpu = (dest_cpu++)%ncpus;
		if(caller==p) {
			cpu = -1;
		}

		/* Try to schedule the process. */
		if((r = sched_proc(p, priority, quantum, cpu, FALSE)) != OK)
			return r;
		p->p_scheduler = NULL;
	} else {
		/* the caller becomes the scheduler */
		p->p_scheduler = caller;
	}

	return(OK);
}

int do_schedctl(struct proc * caller, message * m_ptr)
{
	int proc_nr,res;
	/* check parameter validity */
	const uint32_t flags = m_ptr->m_lsys_krn_schedctl.flags;
	if (flags & ~SCHEDCTL_FLAG_KERNEL) {
		printf("do_schedctl: flags 0x%x invalid, caller=%d\n", 
			flags, caller - proc);
		res = EINVAL;
	} else if (!isokendpt(m_ptr->m_lsys_krn_schedctl.endpoint, &proc_nr)) {
		res = EINVAL;
	} else {
		struct proc *const p = proc_addr(proc_nr);
		lock_proc(p);
		res = do_schedctl_impl(caller,p,m_ptr);
		unlock_proc(p);
	}

	lock_proc(caller);
	return res;
}

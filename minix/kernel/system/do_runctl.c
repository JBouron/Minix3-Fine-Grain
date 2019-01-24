/* The kernel call implemented in this file:
 *   m_type:	SYS_RUNCTL
 *
 * The parameters for this kernel call are:
 *    m1_i1:	RC_ENDPT	process number to control
 *    m1_i2:	RC_ACTION	stop or resume the process
 *    m1_i3:	RC_FLAGS	request flags
 */

#include "kernel/system.h"
#include <assert.h>

#if USE_RUNCTL

/*===========================================================================*
 *				  do_runctl				     *
 *===========================================================================*/
static int do_runctl_impl(struct proc * rp, message * m_ptr)
{
/* Control a process's RTS_PROC_STOP flag. Used for process management.
 * If the process is queued sending a message or stopped for system call
 * tracing, and the RC_DELAY request flag is given, set MF_SIG_DELAY instead
 * of RTS_PROC_STOP, and send a SIGSNDELAY signal later when the process is done
 * sending (ending the delay). Used by PM for safe signal delivery.
 */
  int action, flags;

  action = m_ptr->RC_ACTION;
  flags = m_ptr->RC_FLAGS;

  /* Is the target sending or syscall-traced? Then set MF_SIG_DELAY instead.
   * Do this only when the RC_DELAY flag is set in the request flags field.
   * The process will not become runnable before PM has called SYS_ENDKSIG.
   * Note that asynchronous messages are not covered: a process using SENDA
   * should not also install signal handlers *and* expect POSIX compliance.
   */

  if (action == RC_STOP && (flags & RC_DELAY)) {
	if (RTS_ISSET(rp, RTS_SENDING) || (rp->p_misc_flags & MF_SC_DEFER))
		rp->p_misc_flags |= MF_SIG_DELAY;

	if (rp->p_misc_flags & MF_SIG_DELAY)
		return (EBUSY);
  }

  /* Either set or clear the stop flag. */
  switch (action) {
  case RC_STOP:
#if CONFIG_SMP
	  /* check if we must stop a process on a different CPU */
	  if (rp->p_cpu != cpuid) {
		  smp_schedule_stop_proc(rp);
		  break;
	  }
#endif
	  RTS_SET(rp, RTS_PROC_STOP);
	break;
  case RC_RESUME:
	assert(RTS_ISSET(rp, RTS_PROC_STOP));
	RTS_UNSET(rp, RTS_PROC_STOP);
	break;
  default:
	return(EINVAL);
  }

  return(OK);
}

int do_runctl(struct proc * caller, message * m_ptr)
{
	int res,proc_nr;
	struct proc *rp;
	/* Extract the message parameters and do sanity checking. */
	if (!isokendpt(m_ptr->RC_ENDPT, &proc_nr)) {
		res =(EINVAL);
	} else if (iskerneln(proc_nr)) {
		res = (EPERM);
	} else {
		rp = proc_addr(proc_nr);

		lock_proc(rp);
		res = do_runctl_impl(rp,m_ptr);
		unlock_proc(rp);
	}

	lock_proc(caller);
	return res;
}

#endif /* USE_RUNCTL */

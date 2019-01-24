/* The system call implemented in this file:
 *   m_type:	SYS_IOPENABLE
 *
 * The parameters for this system call are:
 *   m_lsys_krn_sys_iopenable.endpt	(process to give I/O Protection Level bits)
 *
 * Author:
 *    Jorrit N. Herder <jnherder@cs.vu.nl>
 */

#include "kernel/system.h"
#include <minix/endpoint.h>

#include "arch_proto.h"

/*===========================================================================*
 *			        do_iopenable				     *
 *===========================================================================*/
int do_iopenable(struct proc * caller, message * m_ptr)
{
  int proc_nr;

  if (m_ptr->m_lsys_krn_sys_iopenable.endpt == SELF) {
	okendpt(caller->p_endpoint, &proc_nr);
  } else if(!isokendpt(m_ptr->m_lsys_krn_sys_iopenable.endpt, &proc_nr))
	goto fail;

  struct proc *const rp = proc_addr(proc_nr);
  lock_proc(rp);
  enable_iop(rp);
  unlock_proc(rp);

  lock_proc(caller);
  return(OK);
fail:
  lock_proc(caller);
  return EINVAL;
}



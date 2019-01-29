/* The kernel call implemented in this file:
 *   m_type:	SYS_VIRCOPY, SYS_PHYSCOPY
 *
 * The parameters for this kernel call are:
 *   m_lsys_krn_sys_copy.src_addr		source offset within segment
 *   m_lsys_krn_sys_copy.src_endpt		source process number
 *   m_lsys_krn_sys_copy.dst_addr		destination offset within segment
 *   m_lsys_krn_sys_copy.dst_endpt		destination process number
 *   m_lsys_krn_sys_copy.nr_bytes		number of bytes to copy
 *   m_lsys_krn_sys_copy.flags
 */

#include "kernel/system.h"
#include "kernel/vm.h"
#include <assert.h>

#if (USE_VIRCOPY || USE_PHYSCOPY)

/*===========================================================================*
 *				do_copy					     *
 *===========================================================================*/
int do_copy(struct proc * caller, message * m_ptr)
{
	/* Handle sys_vircopy() and sys_physcopy().  Copy data using virtual or
	 * physical addressing. Although a single handler function is used, there 
	 * are two different kernel calls so that permissions can be checked. 
	 */
	struct vir_addr vir_addr[2];	/* virtual source and destination address */
	struct proc *procs[2] = {0,0};	/* Procs src and dest. */
	phys_bytes bytes;		/* number of bytes to copy */
	int i;

	/* Dismember the command message. */
	vir_addr[_SRC_].proc_nr_e = m_ptr->m_lsys_krn_sys_copy.src_endpt;
	vir_addr[_DST_].proc_nr_e = m_ptr->m_lsys_krn_sys_copy.dst_endpt;

	vir_addr[_SRC_].offset = m_ptr->m_lsys_krn_sys_copy.src_addr;
	vir_addr[_DST_].offset = m_ptr->m_lsys_krn_sys_copy.dst_addr;
	bytes = m_ptr->m_lsys_krn_sys_copy.nr_bytes;

	/* Now do some checks for both the source and destination virtual address.
	 * This is done once for _SRC_, then once for _DST_. 
	 */
	for (i=_SRC_; i<=_DST_; i++) {
		int p;

		/* Check if process number was given implicitly with SELF and is valid. */
		if (vir_addr[i].proc_nr_e == SELF)
			vir_addr[i].proc_nr_e = caller->p_endpoint;

		if (vir_addr[i].proc_nr_e != NONE) {
			if(! isokendpt(vir_addr[i].proc_nr_e, &p)) {
				printf("do_copy: %d: %d not ok endpoint\n", i, vir_addr[i].proc_nr_e);
				lock_proc(caller);
				return(EINVAL); 
			} else {
				procs[i] = proc_addr(p);
			} 
		}
	}

	/* Check for overflow. This would happen for 64K segments and 16-bit 
	 * vir_bytes. Especially copying by the PM on do_fork() is affected. 
	 */
	if (bytes != (phys_bytes) (vir_bytes) bytes) {
		lock_proc(caller);	
		return(E2BIG);
	}

	/* Lock the procs. */
	lock_three_procs(caller,procs[0],procs[1]);

	/* Now try to make the actual virtual copy. */
	int r;
	if(m_ptr->m_lsys_krn_sys_copy.flags & CP_FLAG_TRY) {
		assert(caller->p_endpoint == VFS_PROC_NR);
		r = virtual_copy(&vir_addr[_SRC_], &vir_addr[_DST_], bytes);
		if(r == EFAULT_SRC || r == EFAULT_DST)
			r = EFAULT;
	} else {
		r = virtual_copy_vmcheck(caller, &vir_addr[_SRC_], &vir_addr[_DST_], bytes);
	}

	/* Handle the unlocking and return the result. */
	/* Make sure not to unlock caller. */
	if(procs[0]!=caller)
		unlock_proc(procs[0]);

	if(procs[1]!=caller&&procs[1]!=procs[0])
		unlock_proc(procs[1]);

	return r;
}
#endif /* (USE_VIRCOPY || USE_PHYSCOPY) */

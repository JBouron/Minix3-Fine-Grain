/* The kernel call implemented in this file:
 *   m_type:	SYS_DIAGCTL
 *
 * The parameters for this kernel call are:
 * 	m_lsys_krn_sys_diagctl.code	request
 * and then request-specific arguments in
 *	m_lsys_krn_sys_diagctl.buf
 *	m_lsys_krn_sys_diagctl.len
 *	m_lsys_krn_sys_diagctl.endpt
 */

#include "kernel/system.h"


/*===========================================================================*
 *			        do_diagctl				     *
 *===========================================================================*/
int do_diagctl(struct proc * caller, message * m_ptr)
{
  vir_bytes len, buf;
  char mybuf[DIAG_BUFSIZE];
  int s, i, proc_nr;

  switch (m_ptr->m_lsys_krn_sys_diagctl.code) {
    case DIAGCTL_CODE_DIAG:
        buf = m_ptr->m_lsys_krn_sys_diagctl.buf;
        len = m_ptr->m_lsys_krn_sys_diagctl.len;
	lock_proc(caller);
	if(len < 1 || len > DIAG_BUFSIZE) {
		printf("do_diagctl: diag for %d: len %d out of range\n",
			caller->p_endpoint, len);
		return EINVAL;
	}
	if((s=data_copy_vmcheck(caller, caller->p_endpoint, buf, KERNEL,
					(vir_bytes) mybuf, len)) != OK) {
		printf("do_diagctl: diag for %d: len %d: copy failed: %d\n",
			caller->p_endpoint, len, s);
		return s;
	}
	for(i = 0; i < len; i++)
		kputc(mybuf[i]);
	kputc(END_OF_KMESS);
	return OK;
    case DIAGCTL_CODE_STACKTRACE:
	if(!isokendpt(m_ptr->m_lsys_krn_sys_diagctl.endpt, &proc_nr)) {
		lock_proc(caller);
		return EINVAL;
	} else {
		struct proc *rp = proc_addr(proc_nr);
		lock_proc(rp);
		proc_stacktrace(rp);
		unlock_proc(rp);
		lock_proc(caller);
		return OK;
	}
    case DIAGCTL_CODE_REGISTER:
	lock_proc(caller);
	if (!(priv(caller)->s_flags & SYS_PROC))
		return EPERM;
	priv(caller)->s_diag_sig = TRUE;
	/* If the message log is not empty, send a first notification
	 * immediately. After bootup the log is basically never empty.
	 */
	if (kmess.km_size > 0 && !kinfo.do_serial_debug)
		send_sig_deferred(caller->p_endpoint, SIGKMESS);
	return OK;
    case DIAGCTL_CODE_UNREGISTER:
	lock_proc(caller);
	if (!(priv(caller)->s_flags & SYS_PROC))
		return EPERM;
	priv(caller)->s_diag_sig = FALSE;
	return OK;
    default:
	lock_proc(caller);
	printf("do_diagctl: invalid request %d\n", m_ptr->m_lsys_krn_sys_diagctl.code);
        return(EINVAL);
  }
}


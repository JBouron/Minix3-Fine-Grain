/* This file handles signals, which are asynchronous events and are generally
 * a messy and unpleasant business.  Signals can be generated by the KILL
 * system call, or from the keyboard (SIGINT) or from the clock (SIGALRM).
 * In all cases control eventually passes to check_sig() to see which processes
 * can be signaled.  The actual signaling is done by sig_proc().
 *
 * The entry points into this file are:
 *   do_sigaction:	perform the SIGACTION system call
 *   do_sigpending:	perform the SIGPENDING system call
 *   do_sigprocmask:	perform the SIGPROCMASK system call
 *   do_sigreturn:	perform the SIGRETURN system call
 *   do_sigsuspend:	perform the SIGSUSPEND system call
 *   do_kill:		perform the KILL system call
 *   process_ksig:	process a signal an behalf of the kernel
 *   sig_proc:		interrupt or terminate a signaled process
 *   check_sig:		check which processes to signal with sig_proc()
 *   check_pending:	check if a pending signal can now be delivered
 *   restart_sigs: 	restart signal work after finishing a VFS call
 */

#include "pm.h"
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <minix/callnr.h>
#include <minix/endpoint.h>
#include <minix/com.h>
#include <minix/vm.h>
#include <signal.h>
#include <sys/resource.h>
#include <assert.h>
#include "mproc.h"

static int unpause(struct mproc *rmp);
static int sig_send(struct mproc *rmp, int signo);
static void sig_proc_exit(struct mproc *rmp, int signo);

/*===========================================================================*
 *				do_sigaction				     *
 *===========================================================================*/
int do_sigaction(void)
{
  int r, sig_nr;
  struct sigaction svec;
  struct sigaction *svp;

  assert(!(mp->mp_flags & (PROC_STOPPED | VFS_CALL | UNPAUSED)));

  sig_nr = m_in.m_lc_pm_sig.nr;
  if (sig_nr == SIGKILL) return(OK);
  if (sig_nr < 1 || sig_nr >= _NSIG) return(EINVAL);

  svp = &mp->mp_sigact[sig_nr];
  if (m_in.m_lc_pm_sig.oact != 0) {
	r = sys_datacopy(PM_PROC_NR,(vir_bytes) svp, who_e,
		m_in.m_lc_pm_sig.oact, (phys_bytes) sizeof(svec));
	if (r != OK) return(r);
  }

  if (m_in.m_lc_pm_sig.act == 0)
  	return(OK);

  /* Read in the sigaction structure. */
  r = sys_datacopy(who_e, m_in.m_lc_pm_sig.act, PM_PROC_NR, (vir_bytes) &svec,
	  (phys_bytes) sizeof(svec));
  if (r != OK) return(r);

  if (svec.sa_handler == SIG_IGN) {
	sigaddset(&mp->mp_ignore, sig_nr);
	sigdelset(&mp->mp_sigpending, sig_nr);
	sigdelset(&mp->mp_ksigpending, sig_nr);
	sigdelset(&mp->mp_catch, sig_nr);
  } else if (svec.sa_handler == SIG_DFL) {
	sigdelset(&mp->mp_ignore, sig_nr);
	sigdelset(&mp->mp_catch, sig_nr);
  } else {
	sigdelset(&mp->mp_ignore, sig_nr);
	sigaddset(&mp->mp_catch, sig_nr);
  }
  mp->mp_sigact[sig_nr].sa_handler = svec.sa_handler;
  sigdelset(&svec.sa_mask, SIGKILL);
  sigdelset(&svec.sa_mask, SIGSTOP);
  mp->mp_sigact[sig_nr].sa_mask = svec.sa_mask;
  mp->mp_sigact[sig_nr].sa_flags = svec.sa_flags;
  mp->mp_sigreturn = m_in.m_lc_pm_sig.ret;
  return(OK);
}

/*===========================================================================*
 *				do_sigpending                                *
 *===========================================================================*/
int do_sigpending(void)
{
  assert(!(mp->mp_flags & (PROC_STOPPED | VFS_CALL | UNPAUSED)));

  mp->mp_reply.m_pm_lc_sigset.set = mp->mp_sigpending;
  return OK;
}

/*===========================================================================*
 *				do_sigprocmask                               *
 *===========================================================================*/
int do_sigprocmask(void)
{
/* Note that the library interface passes the actual mask in sigmask_set,
 * not a pointer to the mask, in order to save a copy.  Similarly,
 * the old mask is placed in the return message which the library
 * interface copies (if requested) to the user specified address.
 *
 * The library interface must set SIG_INQUIRE if the 'act' argument
 * is NULL.
 *
 * KILL and STOP can't be masked.
 */
  sigset_t set;
  int i;

  assert(!(mp->mp_flags & (PROC_STOPPED | VFS_CALL | UNPAUSED)));

  set = m_in.m_lc_pm_sigset.set;
  mp->mp_reply.m_pm_lc_sigset.set = mp->mp_sigmask;

  switch (m_in.m_lc_pm_sigset.how) {
      case SIG_BLOCK:
	sigdelset(&set, SIGKILL);
	sigdelset(&set, SIGSTOP);
	for (i = 1; i < _NSIG; i++) {
		if (sigismember(&set, i))
			sigaddset(&mp->mp_sigmask, i);
	}
	break;

      case SIG_UNBLOCK:
	for (i = 1; i < _NSIG; i++) {
		if (sigismember(&set, i))
			sigdelset(&mp->mp_sigmask, i);
	}
	check_pending(mp);
	break;

      case SIG_SETMASK:
	sigdelset(&set, SIGKILL);
	sigdelset(&set, SIGSTOP);
	mp->mp_sigmask = set;
	check_pending(mp);
	break;

      case SIG_INQUIRE:
	break;

      default:
	return(EINVAL);
	break;
  }
  return OK;
}

/*===========================================================================*
 *				do_sigsuspend                                *
 *===========================================================================*/
int do_sigsuspend(void)
{
  assert(!(mp->mp_flags & (PROC_STOPPED | VFS_CALL | UNPAUSED)));

  mp->mp_sigmask2 = mp->mp_sigmask;	/* save the old mask */
  mp->mp_sigmask = m_in.m_lc_pm_sigset.set;
  sigdelset(&mp->mp_sigmask, SIGKILL);
  sigdelset(&mp->mp_sigmask, SIGSTOP);
  mp->mp_flags |= SIGSUSPENDED;
  check_pending(mp);
  return(SUSPEND);
}

/*===========================================================================*
 *				do_sigreturn				     *
 *===========================================================================*/
int do_sigreturn(void)
{
/* A user signal handler is done.  Restore context and check for
 * pending unblocked signals.
 */
  int r;

  assert(!(mp->mp_flags & (PROC_STOPPED | VFS_CALL | UNPAUSED)));

  mp->mp_sigmask = m_in.m_lc_pm_sigset.set;
  sigdelset(&mp->mp_sigmask, SIGKILL);
  sigdelset(&mp->mp_sigmask, SIGSTOP);

  r = sys_sigreturn(who_e, (struct sigmsg *)m_in.m_lc_pm_sigset.ctx);
  check_pending(mp);
  return(r);
}

/*===========================================================================*
 *				do_kill					     *
 *===========================================================================*/
int do_kill(void)
{
/* Perform the kill(pid, signo) system call. */

  return check_sig(m_in.m_lc_pm_sig.pid, m_in.m_lc_pm_sig.nr, FALSE /* ksig */);
}

/*===========================================================================*
 *			      do_srv_kill				     *
 *===========================================================================*/
int do_srv_kill(void)
{
/* Perform the srv_kill(pid, signo) system call. */

  /* Only RS is allowed to use srv_kill. */
  if (mp->mp_endpoint != RS_PROC_NR)
	return EPERM;

  /* Pretend the signal comes from the kernel when RS wants to deliver a signal
   * to a system process. RS sends a SIGKILL when it wants to perform cleanup.
   * In that case, ksig == TRUE forces PM to exit the process immediately.
   */
  return check_sig(m_in.m_rs_pm_srv_kill.pid, m_in.m_rs_pm_srv_kill.nr,
	  TRUE /* ksig */);
}

/*===========================================================================*
 *				stop_proc				     *
 *===========================================================================*/
static int stop_proc(struct mproc *rmp, int may_delay)
{
/* Try to stop the given process in the kernel. If successful, mark the process
 * as stopped and return TRUE.  If the process is still busy sending a message,
 * the behavior depends on the 'may_delay' parameter. If set, the process will
 * be marked as having a delay call pending, and the function returns FALSE. If
 * not set, the caller already knows that the process has no delay call, and PM
 * will panic.
 */
  int r;

  assert(!(rmp->mp_flags & (PROC_STOPPED | DELAY_CALL | UNPAUSED)));

  r = sys_delay_stop(rmp->mp_endpoint);

  /* If the process is still busy sending a message, the kernel will give us
   * EBUSY now and send a SIGSNDELAY to the process as soon as sending is done.
   */
  switch (r) {
  case OK:
	rmp->mp_flags |= PROC_STOPPED;

	return TRUE;

  case EBUSY:
	if (!may_delay)
		panic("stop_proc: unexpected delay call");

	rmp->mp_flags |= DELAY_CALL;

	return FALSE;

  default:
	panic("sys_delay_stop failed: %d", r);
  }
}

/*===========================================================================*
 *				try_resume_proc				     *
 *===========================================================================*/
static void try_resume_proc(struct mproc *rmp)
{
/* Resume the given process if possible. */
  int r;

  assert(rmp->mp_flags & PROC_STOPPED);

  /* If the process is blocked on a VFS call, do not resume it now. Most likely    * it will be unpausing, in which case the process must remain stopped.
   * Otherwise, it will still be resumed once the VFS call returns. If the
   * process has died, do not resume it either.
   */
  if (rmp->mp_flags & (VFS_CALL | EXITING))
	return;

  if ((r = sys_resume(rmp->mp_endpoint)) != OK)
	panic("sys_resume failed: %d", r);

  /* Also unset the unpaused flag. We can safely assume that a stopped process
   * need only be unpaused once, but once it is resumed, all bets are off.
   */
  rmp->mp_flags &= ~(PROC_STOPPED | UNPAUSED);
}

/*===========================================================================*
 *				process_ksig				     *
 *===========================================================================*/
int process_ksig(endpoint_t proc_nr_e, int signo)
{
  register struct mproc *rmp;
  int proc_nr;
  pid_t proc_id, id;

  if(pm_isokendpt(proc_nr_e, &proc_nr) != OK) {
	printf("PM: process_ksig: %d?? not ok\n", proc_nr_e);
	return EDEADEPT; /* process is gone. */
  }
  rmp = &mproc[proc_nr];
  if ((rmp->mp_flags & (IN_USE | EXITING)) != IN_USE) {
#if 0
	printf("PM: process_ksig: %d?? exiting / not in use\n", proc_nr_e);
#endif
	return EDEADEPT; /* process is gone. */
  }
  proc_id = rmp->mp_pid;
  mp = &mproc[0];			/* pretend signals are from PM */
  mp->mp_procgrp = rmp->mp_procgrp;	/* get process group right */

  /* For SIGVTALRM and SIGPROF, see if we need to restart a
   * virtual timer. For SIGINT, SIGINFO, SIGWINCH and SIGQUIT, use proc_id 0
   * to indicate a broadcast to the recipient's process group.  For
   * SIGKILL, use proc_id -1 to indicate a systemwide broadcast.
   */
  switch (signo) {
      case SIGINT:
      case SIGQUIT:
      case SIGWINCH:
      case SIGINFO:
  	id = 0; break;	/* broadcast to process group */
      case SIGVTALRM:
      case SIGPROF:
      	check_vtimer(proc_nr, signo);
      	/* fall-through */
      default:
  	id = proc_id;
  	break;
  }
  check_sig(id, signo, TRUE /* ksig */);

  /* If SIGSNDELAY is set, an earlier sys_stop() failed because the process was
   * still sending, and the kernel hereby tells us that the process is now done
   * with that. We can now try to resume what we planned to do in the first
   * place: set up a signal handler. However, the process's message may have
   * been a call to PM, in which case the process may have changed any of its
   * signal settings. The process may also have forked, exited etcetera.
   */
  if (signo == SIGSNDELAY && (rmp->mp_flags & DELAY_CALL)) {
	/* When getting SIGSNDELAY, the process is stopped at least until the
	 * receipt of the SIGSNDELAY signal is acknowledged to the kernel. The
	 * process is not stopped on PROC_STOP in the kernel. However, now that
	 * there is no longer a delay call, stop_proc() is guaranteed to
	 * succeed immediately.
	 */
	rmp->mp_flags &= ~DELAY_CALL;

	assert(!(rmp->mp_flags & PROC_STOPPED));

	/* If the delay call was to PM, it may have resulted in a VFS call. In
	 * that case, we must wait with further signal processing until VFS has
	 * replied. Stop the process.
	 */
	if (rmp->mp_flags & VFS_CALL) {
		stop_proc(rmp, FALSE /*may_delay*/);

		return OK;
	}

	/* Process as many normal signals as possible. */
	check_pending(rmp);

	assert(!(rmp->mp_flags & DELAY_CALL));
  }
  
  /* See if the process is still alive */
  if ((mproc[proc_nr].mp_flags & (IN_USE | EXITING)) == IN_USE)  {
      return OK; /* signal has been delivered */
  }
  else {
      return EDEADEPT; /* process is gone */
  }
}

/*===========================================================================*
 *				sig_proc				     *
 *===========================================================================*/
void sig_proc(rmp, signo, trace, ksig)
register struct mproc *rmp;	/* pointer to the process to be signaled */
int signo;			/* signal to send to process (1 to _NSIG-1) */
int trace;			/* pass signal to tracer first? */
int ksig;			/* non-zero means signal comes from kernel  */
{
/* Send a signal to a process.  Check to see if the signal is to be caught,
 * ignored, tranformed into a message (for system processes) or blocked.  
 *  - If the signal is to be transformed into a message, request the KERNEL to
 * send the target process a system notification with the pending signal as an 
 * argument. 
 *  - If the signal is to be caught, request the KERNEL to push a sigcontext 
 * structure and a sigframe structure onto the catcher's stack.  Also, KERNEL 
 * will reset the program counter and stack pointer, so that when the process 
 * next runs, it will be executing the signal handler. When the signal handler 
 * returns,  sigreturn(2) will be called.  Then KERNEL will restore the signal 
 * context from the sigcontext structure.
 * If there is insufficient stack space, kill the process.
 */
  int slot, badignore;

  slot = (int) (rmp - mproc);
  if ((rmp->mp_flags & (IN_USE | EXITING)) != IN_USE) {
	panic("PM: signal %d sent to exiting process %d\n", signo, slot);
  }

  if (trace == TRUE && rmp->mp_tracer != NO_TRACER && signo != SIGKILL) {
	/* Signal should be passed to the debugger first.
	 * This happens before any checks on block/ignore masks; otherwise,
	 * the process itself could block/ignore debugger signals.
	 */

	sigaddset(&rmp->mp_sigtrace, signo);

	if (!(rmp->mp_flags & TRACE_STOPPED))
		trace_stop(rmp, signo);	/* a signal causes it to stop */

	return;
  }

  if (rmp->mp_flags & VFS_CALL) {
	sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		sigaddset(&rmp->mp_ksigpending, signo);

	/* Process the signal once VFS replies. Stop the process in the
	 * meantime, so that it cannot make another call after the VFS reply
	 * comes in but before we look at its signals again. Since we always
	 * stop the process to deliver signals during a VFS call, the
	 * PROC_STOPPED flag doubles as an indicator in restart_sigs() that
	 * signals must be rechecked after a VFS reply comes in.
	 */
	if (!(rmp->mp_flags & (PROC_STOPPED | DELAY_CALL))) {
		/* If a VFS call is ongoing and the process is not yet stopped,
		 * the process must have made a call to PM. Therefore, there
		 * can be no delay calls in this case.
		 */
		stop_proc(rmp, FALSE /*delay_call*/);
	}
	return;
  }

  /* Handle system signals for system processes first. */
  if(rmp->mp_flags & PRIV_PROC) {
   	/* Always skip signals for PM (only necessary when broadcasting). */
   	if(rmp->mp_endpoint == PM_PROC_NR) {
 		return;
   	}

   	/* System signals have always to go through the kernel first to let it
   	 * pick the right signal manager. If PM is the assigned signal manager,
   	 * the signal will come back and will actually be processed.
   	 */
   	if(!ksig) {
 		sys_kill(rmp->mp_endpoint, signo);
 		return;
   	}

  	/* Print stacktrace if necessary. */
  	if(SIGS_IS_STACKTRACE(signo)) {
		sys_diagctl_stacktrace(rmp->mp_endpoint);
  	}

  	if(!SIGS_IS_TERMINATION(signo)) {
		/* Translate every non-termination sys signal into a message. */
		message m;
		m.m_type = SIGS_SIGNAL_RECEIVED;
		m.SIGS_SIG_NUM = signo;
		asynsend3(rmp->mp_endpoint, &m, AMF_NOREPLY);
	}
	else {
		/* Exit the process in case of termination system signal. */
		sig_proc_exit(rmp, signo);
	}
	return;
  }

  /* Handle user processes now. See if the signal cannot be safely ignored. */
  badignore = ksig && sigismember(&noign_sset, signo) && (
	  sigismember(&rmp->mp_ignore, signo) ||
	  sigismember(&rmp->mp_sigmask, signo));

  if (!badignore && sigismember(&rmp->mp_ignore, signo)) { 
	/* Signal should be ignored. */
	return;
  }
  if (!badignore && sigismember(&rmp->mp_sigmask, signo)) {
	/* Signal should be blocked. */
	sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		sigaddset(&rmp->mp_ksigpending, signo);
	return;
  }

  if ((rmp->mp_flags & TRACE_STOPPED) && signo != SIGKILL) {
	/* If the process is stopped for a debugger, do not deliver any signals
	 * (except SIGKILL) in order not to confuse the debugger. The signals
	 * will be delivered using the check_pending() calls in do_trace().
	 */
	sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		sigaddset(&rmp->mp_ksigpending, signo);
	return;
  }
  if (!badignore && sigismember(&rmp->mp_catch, signo)) {
	/* Signal is caught. First interrupt the process's current call, if
	 * applicable. This may involve a roundtrip to VFS, in which case we'll
	 * have to check back later.
	 */
	if (!unpause(rmp)) {
		/* not yet unpaused; continue later */
		sigaddset(&rmp->mp_sigpending, signo);
		if(ksig)
			sigaddset(&rmp->mp_ksigpending, signo);

		return;
	}

	/* Then send the actual signal to the process, by setting up a signal
	 * handler.
	 */
	if (sig_send(rmp, signo))
		return;

	/* We were unable to spawn a signal handler. Kill the process. */
	printf("PM: %d can't catch signal %d - killing\n",
		rmp->mp_pid, signo);
  }
  else if (!badignore && sigismember(&ign_sset, signo)) {
	/* Signal defaults to being ignored. */
	return;
  }

  /* Terminate process */
  sig_proc_exit(rmp, signo);
}

/*===========================================================================*
 *				sig_proc_exit				     *
 *===========================================================================*/
static void sig_proc_exit(rmp, signo)
struct mproc *rmp;		/* process that must exit */
int signo;			/* signal that caused termination */
{
  rmp->mp_sigstatus = (char) signo;
  if (sigismember(&core_sset, signo)) {
	if(!(rmp->mp_flags & PRIV_PROC)) {
		printf("PM: coredump signal %d for %d / %s\n", signo,
			rmp->mp_pid, rmp->mp_name);
		sys_diagctl_stacktrace(rmp->mp_endpoint);
	}
	exit_proc(rmp, 0, TRUE /*dump_core*/);
  }
  else {
  	exit_proc(rmp, 0, FALSE /*dump_core*/);
  }
}

/*===========================================================================*
 *				check_sig				     *
 *===========================================================================*/
int check_sig(proc_id, signo, ksig)
pid_t proc_id;			/* pid of proc to sig, or 0 or -1, or -pgrp */
int signo;			/* signal to send to process (0 to _NSIG-1) */
int ksig;			/* non-zero means signal comes from kernel  */
{
/* Check to see if it is possible to send a signal.  The signal may have to be
 * sent to a group of processes.  This routine is invoked by the KILL system
 * call, and also when the kernel catches a DEL or other signal.
 */

  register struct mproc *rmp;
  int count;			/* count # of signals sent */
  int error_code;

  if (signo < 0 || signo >= _NSIG) return(EINVAL);

  /* Return EINVAL for attempts to send SIGKILL to INIT alone. */
  if (proc_id == INIT_PID && signo == SIGKILL) return(EINVAL);

  /* Signal RS first when broadcasting SIGTERM. */
  if (proc_id == -1 && signo == SIGTERM)
      sys_kill(RS_PROC_NR, signo);

  /* Search the proc table for processes to signal. Start from the end of the
   * table to analyze core system processes at the end when broadcasting.
   * (See forkexit.c about pid magic.)
   */
  count = 0;
  error_code = ESRCH;
  for (rmp = &mproc[NR_PROCS-1]; rmp >= &mproc[0]; rmp--) {
	if (!(rmp->mp_flags & IN_USE)) continue;

	/* Check for selection. */
	if (proc_id > 0 && proc_id != rmp->mp_pid) continue;
	if (proc_id == 0 && mp->mp_procgrp != rmp->mp_procgrp) continue;
	if (proc_id == -1 && rmp->mp_pid <= INIT_PID) continue;
	if (proc_id < -1 && rmp->mp_procgrp != -proc_id) continue;

	/* Do not kill servers and drivers when broadcasting SIGKILL. */
	if (proc_id == -1 && signo == SIGKILL &&
		(rmp->mp_flags & PRIV_PROC)) continue;

	/* Skip VM entirely as it might lead to a deadlock with its signal
	 * manager if the manager page faults at the same time.
	 */
	if (rmp->mp_endpoint == VM_PROC_NR) continue;

	/* Disallow lethal signals sent by user processes to sys processes. */
	if (!ksig && SIGS_IS_LETHAL(signo) && (rmp->mp_flags & PRIV_PROC)) {
	    error_code = EPERM;
	    continue;
	}

	/* Check for permission. */
	if (mp->mp_effuid != SUPER_USER
	    && mp->mp_realuid != rmp->mp_realuid
	    && mp->mp_effuid != rmp->mp_realuid
	    && mp->mp_realuid != rmp->mp_effuid
	    && mp->mp_effuid != rmp->mp_effuid) {
		error_code = EPERM;
		continue;
	}

	count++;
	if (signo == 0 || (rmp->mp_flags & EXITING)) continue;

	/* 'sig_proc' will handle the disposition of the signal.  The
	 * signal may be caught, blocked, ignored, or cause process
	 * termination, possibly with core dump.
	 */
	sig_proc(rmp, signo, TRUE /*trace*/, ksig);

	if (proc_id > 0) break;	/* only one process being signaled */
  }

  /* If the calling process has killed itself, don't reply. */
  if ((mp->mp_flags & (IN_USE | EXITING)) != IN_USE) return(SUSPEND);
  return(count > 0 ? OK : error_code);
}

/*===========================================================================*
 *				check_pending				     *
 *===========================================================================*/
void check_pending(rmp)
register struct mproc *rmp;
{
  /* Check to see if any pending signals have been unblocked. Deliver as many
   * of them as we can, until we have to wait for a reply from VFS first.
   *
   * There are several places in this file where the signal mask is
   * changed.  At each such place, check_pending() should be called to
   * check for newly unblocked signals.
   */
  int i;
  int ksig;

  for (i = 1; i < _NSIG; i++) {
	if (sigismember(&rmp->mp_sigpending, i) &&
		!sigismember(&rmp->mp_sigmask, i)) {
		ksig = sigismember(&rmp->mp_ksigpending, i);
		sigdelset(&rmp->mp_sigpending, i);
		sigdelset(&rmp->mp_ksigpending, i);
		sig_proc(rmp, i, FALSE /*trace*/, ksig);

		if (rmp->mp_flags & VFS_CALL) {
			/* Signals must be rechecked upon return from the new
			 * VFS call, unless the process was killed. In both
			 * cases, the process is stopped.
			 */
			assert(rmp->mp_flags & PROC_STOPPED);
			break;
		}
	}
  }
}

/*===========================================================================*
 *				restart_sigs				     *
 *===========================================================================*/
void restart_sigs(rmp)
struct mproc *rmp;
{
/* VFS has replied to a request from us; do signal-related work.
 */

  if (rmp->mp_flags & (VFS_CALL | EXITING)) return;

  if (rmp->mp_flags & TRACE_EXIT) {
	/* Tracer requested exit with specific exit value */
	exit_proc(rmp, rmp->mp_exitstatus, FALSE /*dump_core*/);
  }
  else if (rmp->mp_flags & PROC_STOPPED) {
	/* If a signal arrives while we are performing a VFS call, the process
	 * will always be stopped immediately. Thus, if the process is stopped
	 * once the reply from VFS arrives, we might have to check signals.
	 */
	assert(!(rmp->mp_flags & DELAY_CALL));

	/* We saved signal(s) for after finishing a VFS call. Deal with this.
	 * PROC_STOPPED remains set to indicate the process is still stopped.
	 */
	check_pending(rmp);

	/* Resume the process now, unless there is a reason not to. */
	try_resume_proc(rmp);
  }
}

/*===========================================================================*
 *				unpause					     *
 *===========================================================================*/
static int unpause(rmp)
struct mproc *rmp;		/* which process */
{
/* A signal is to be sent to a process.  If that process is hanging on a
 * system call, the system call must be terminated with EINTR.  First check if
 * the process is hanging on an PM call.  If not, tell VFS, so it can check for
 * interruptible calls such as READs and WRITEs from pipes, ttys and the like.
 */
  message m;

  assert(!(rmp->mp_flags & VFS_CALL));

  /* If the UNPAUSED flag is set, VFS replied to an earlier unpause request. */
  if (rmp->mp_flags & UNPAUSED) {
	assert((rmp->mp_flags & (DELAY_CALL | PROC_STOPPED)) == PROC_STOPPED);

	return TRUE;
  }

  /* If the process is already stopping, don't do anything now. */
  if (rmp->mp_flags & DELAY_CALL)
	return FALSE;

  /* Check to see if process is hanging on a WAIT or SIGSUSPEND call. */
  if (rmp->mp_flags & (WAITING | SIGSUSPENDED)) {
	/* Stop the process from running. Do not interrupt the actual call yet.
	 * sig_send() will interrupt the call and resume the process afterward.
	 * No delay calls: we know for a fact that the process called us.
	 */
	stop_proc(rmp, FALSE /*may_delay*/);

	return TRUE;
  }

  /* Not paused in PM. Let VFS try to unpause the process. The process needs to
   * be stopped for this. If it is not already stopped, try to stop it now. If
   * that does not succeed immediately, postpone signal delivery.
   */
  if (!(rmp->mp_flags & PROC_STOPPED) && !stop_proc(rmp, TRUE /*may_delay*/))
	return FALSE;

  memset(&m, 0, sizeof(m));
  m.m_type = VFS_PM_UNPAUSE;
  m.VFS_PM_ENDPT = rmp->mp_endpoint;

  tell_vfs(rmp, &m);

  /* Also tell VM. */
  vm_notify_sig_wrapper(rmp->mp_endpoint);

  return FALSE;
}

/*===========================================================================*
 *				sig_send				     *
 *===========================================================================*/
static int sig_send(rmp, signo)
struct mproc *rmp;		/* what process to spawn a signal handler in */
int signo;			/* signal to send to process (1 to _NSIG-1) */
{
/* The process is supposed to catch this signal. Spawn a signal handler.
 * Return TRUE if this succeeded, FALSE otherwise.
 */
  struct sigmsg sigmsg;
  int i, r, sigflags, slot;

  assert(rmp->mp_flags & PROC_STOPPED);

  sigflags = rmp->mp_sigact[signo].sa_flags;
  slot = (int) (rmp - mproc);

  if (rmp->mp_flags & SIGSUSPENDED)
	sigmsg.sm_mask = rmp->mp_sigmask2;
  else
	sigmsg.sm_mask = rmp->mp_sigmask;
  sigmsg.sm_signo = signo;
  sigmsg.sm_sighandler =
	(vir_bytes) rmp->mp_sigact[signo].sa_handler;
  sigmsg.sm_sigreturn = rmp->mp_sigreturn;
  for (i = 1; i < _NSIG; i++) {
	if (sigismember(&rmp->mp_sigact[signo].sa_mask, i))
		sigaddset(&rmp->mp_sigmask, i);
  }

  if (sigflags & SA_NODEFER)
	sigdelset(&rmp->mp_sigmask, signo);
  else
	sigaddset(&rmp->mp_sigmask, signo);

  if (sigflags & SA_RESETHAND) {
	sigdelset(&rmp->mp_catch, signo);
	rmp->mp_sigact[signo].sa_handler = SIG_DFL;
  }
  sigdelset(&rmp->mp_sigpending, signo);
  sigdelset(&rmp->mp_ksigpending, signo);

  /* Ask the kernel to deliver the signal */
  r = sys_sigsend(rmp->mp_endpoint, &sigmsg);
  /* sys_sigsend can fail legitimately with EFAULT or ENOMEM if the process
   * memory can't accommodate the signal handler.  The target process will be
   * killed in that case, so do not bother interrupting or resuming it.
   */
  if(r == EFAULT || r == ENOMEM) {
	return(FALSE);
  }
  /* Other errors are unexpected pm/kernel discrepancies. */
  if (r != OK) {
	panic("sys_sigsend failed: %d", r);
  }

  /* Was the process suspended in PM? Then interrupt the blocking call. */
  if (rmp->mp_flags & (WAITING | SIGSUSPENDED)) {
	rmp->mp_flags &= ~(WAITING | SIGSUSPENDED);

	reply(slot, EINTR);

	/* The process must just have been stopped by unpause(), which means
	 * that the UNPAUSE flag is not set.
	 */
	assert(!(rmp->mp_flags & UNPAUSED));

	try_resume_proc(rmp);

	assert(!(rmp->mp_flags & PROC_STOPPED));
  } else {
	/* If the process was not suspended in PM, VFS must first have
	 * confirmed that it has tried to unsuspend any blocking call. Thus, we
	 * got here from restart_sigs() as part of handling PM_UNPAUSE_REPLY,
	 * and restart_sigs() will resume the process later.
	 */
	assert(rmp->mp_flags & UNPAUSED);
  }

  return(TRUE);
}

/*===========================================================================*
 *				vm_notify_sig_wrapper			     *
 *===========================================================================*/
void vm_notify_sig_wrapper(endpoint_t ep)
{
/* get IPC's endpoint,
 * the reason that we directly get the endpoint
 * instead of from DS server is that otherwise
 * it will cause deadlock between PM, VM and DS.
 */
  struct mproc *rmp;
  endpoint_t ipc_ep = 0;

  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
	if (!(rmp->mp_flags & IN_USE))
		continue;
	if (!strcmp(rmp->mp_name, "ipc")) {
		ipc_ep = rmp->mp_endpoint;
		vm_notify_sig(ep, ipc_ep);

		return;
	}
  }
}

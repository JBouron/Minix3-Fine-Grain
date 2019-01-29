/* This file contains essentially all of the process and message handling.
 * Together with "mpx.s" it forms the lowest layer of the MINIX kernel.
 * There is one entry point from the outside:
 *
 *   sys_call: 	      a system call, i.e., the kernel is trapped with an INT
 *
 * Changes:
 *   Aug 19, 2005     rewrote scheduling code  (Jorrit N. Herder)
 *   Jul 25, 2005     rewrote system call handling  (Jorrit N. Herder)
 *   May 26, 2005     rewrote message passing functions  (Jorrit N. Herder)
 *   May 24, 2005     new notification system call  (Jorrit N. Herder)
 *   Oct 28, 2004     nonblocking send and receive calls  (Jorrit N. Herder)
 *
 * The code here is critical to make everything work and is important for the
 * overall performance of the system. A large fraction of the code deals with
 * list manipulation. To make this both easy to understand and fast to execute 
 * pointer pointers are used throughout the code. Pointer pointers prevent
 * exceptions for the head or tail of a linked list. 
 *
 *  node_t *queue, *new_node;	// assume these as global variables
 *  node_t **xpp = &queue; 	// get pointer pointer to head of queue 
 *  while (*xpp != NULL) 	// find last pointer of the linked list
 *      xpp = &(*xpp)->next;	// get pointer to next pointer 
 *  *xpp = new_node;		// now replace the end (the NULL pointer) 
 *  new_node->next = NULL;	// and mark the new end of the list
 * 
 * For example, when adding a new node to the end of the list, one normally 
 * makes an exception for an empty list and looks up the end of the list for 
 * nonempty lists. As shown above, this is not required with pointer pointers.
 */

#include <stddef.h>
#include <signal.h>
#include <assert.h>
#include <string.h>

#include "vm.h"
#include "clock.h"
#include "spinlock.h"
#include "arch_proto.h"
#include "glo.h"

#include <minix/syslib.h>


static int n_remote_deq;
static int n_remote_enq;
/* Scheduling and message passing functions */
static void idle(void);
/**
 * Made public for use in clock.c (for user-space scheduling)
static int mini_send(struct proc *caller_ptr, endpoint_t dst_e, message
	*m_ptr, int flags);
*/
static int mini_sendrec(struct proc *caller_ptr, endpoint_t src,
	message *m_buff_usr, int flags);
static int mini_sendrec_no_lock(struct proc *caller_ptr, struct proc *to,
	message *m_buff_usr, int flags);

static int mini_receive(struct proc *caller_ptr, endpoint_t src,
	message *m_buff_usr, int flags);
static int mini_receive_no_lock(struct proc *caller_ptr, endpoint_t src,
	message *m_buff_usr, int flags);

static int mini_senda(struct proc *caller_ptr, asynmsg_t *table, size_t
	size);
static int deadlock(int function, register struct proc *caller,
	endpoint_t src_dst_e);
static int try_async(struct proc *caller_ptr);
static int try_one(endpoint_t receive_e, struct proc *src_ptr,
	struct proc *dst_ptr);
static struct proc * pick_proc(void);
static void enqueue_head(struct proc *rp);

/* all idles share the same idle_priv structure */
static struct priv idle_priv;

static void set_idle_name(char * name, int n)
{
        int i, c;
        int p_z = 0;

        if (n > 999) 
                n = 999; 

        name[0] = 'i'; 
        name[1] = 'd'; 
        name[2] = 'l'; 
        name[3] = 'e'; 

        for (i = 4, c = 100; c > 0; c /= 10) {
                int digit;

                digit = n / c;  
                n -= digit * c;  

                if (p_z || digit != 0 || c == 1) {
                        p_z = 1;
                        name[i++] = '0' + digit;
                }   
        }    

        name[i] = '\0';

}


#define PICK_ANY	1
#define PICK_HIGHERONLY	2

#define BuildNotifyMessage(m_ptr, src, dst_ptr) \
	memset((m_ptr), 0, sizeof(*(m_ptr)));				\
	(m_ptr)->m_type = NOTIFY_MESSAGE;				\
	(m_ptr)->m_notify.timestamp = get_monotonic();		\
	switch (src) {							\
	case HARDWARE:							\
		(m_ptr)->m_notify.interrupts =			\
			priv(dst_ptr)->s_int_pending;			\
		priv(dst_ptr)->s_int_pending = 0;			\
		break;							\
	case SYSTEM:							\
		memcpy(&(m_ptr)->m_notify.sigset,			\
			&priv(dst_ptr)->s_sig_pending,			\
			sizeof(sigset_t));				\
		sigemptyset(&priv(dst_ptr)->s_sig_pending);		\
		break;							\
	}

static message m_notify_buff = { 0, NOTIFY_MESSAGE };

void proc_init(void)
{
	struct proc * rp;
	struct priv *sp;
	int i;

	/* Clear the process table. Announce each slot as empty and set up
	 * mappings for proc_addr() and proc_nr() macros. Do the same for the
	 * table with privilege structures for the system processes. 
	 */
	for (rp = BEG_PROC_ADDR, i = -NR_TASKS; rp < END_PROC_ADDR; ++rp, ++i) {
		rp->p_rts_flags = RTS_SLOT_FREE;/* initialize free slot */
		rp->p_magic = PMAGIC;
		rp->p_nr = i;			/* proc number from ptr */
		rp->p_endpoint = _ENDPOINT(0, rp->p_nr); /* generation no. 0 */
		rp->p_scheduler = NULL;		/* no user space scheduler */
		rp->p_priority = 0;		/* no priority */
		rp->p_quantum_size_ms = 0;	/* no quantum size */

		rp->p_enqueued = 0;		/* Proc's not enqueued yet. */
		rp->p_deliver_type = MSG_TYPE_NULL;	/* No message received yet. */
		rp->p_sendto_e = NONE;		/* Proc's not blocked sending. */
		rp->p_next_cpu = -1;

		/* arch-specific initialization */
		arch_proc_reset(rp);
	}
	for (sp = BEG_PRIV_ADDR, i = 0; sp < END_PRIV_ADDR; ++sp, ++i) {
		sp->s_proc_nr = NONE;		/* initialize as free */
		sp->s_id = (sys_id_t) i;	/* priv structure index */
		ppriv_addr[i] = sp;		/* priv ptr from number */
		sp->s_sig_mgr = NONE;		/* clear signal managers */
		sp->s_bak_sig_mgr = NONE;
	}

	idle_priv.s_flags = IDL_F;
	/* initialize IDLE structures for every CPU */
	for (i = 0; i < CONFIG_MAX_CPUS; i++) {
		struct proc * ip = get_cpu_var_ptr(i, idle_proc);
		ip->p_endpoint = IDLE;
		ip->p_priv = &idle_priv;
		/* must not let idle ever get scheduled */
		ip->p_rts_flags |= RTS_PROC_STOP;
		set_idle_name(ip->p_name, i);
	}
}

static void switch_address_space_idle(void)
{
#ifdef CONFIG_SMP
	/*
	 * currently we bet that VM is always alive and its pages available so
	 * when the CPU wakes up the kernel is mapped and no surprises happen.
	 * This is only a problem if more than 1 cpus are available
	 */
	switch_address_space(proc_addr(VM_PROC_NR));
#endif
}

/*===========================================================================*
 *				idle					     * 
 *===========================================================================*/
static void idle(void)
{
	struct proc * p;

	/* This function is called whenever there is no work to do.
	 * Halt the CPU, and measure how many timestamp counter ticks are
	 * spent not doing anything. This allows test setups to measure
	 * the CPU utilization of certain workloads with high precision.
	 */

	p = get_cpulocal_var(proc_ptr) = get_cpulocal_var_ptr(idle_proc);
	if (priv(p)->s_flags & BILLABLE)
		get_cpulocal_var(bill_ptr) = p;

	switch_address_space_idle();

#ifdef CONFIG_SMP
	/* we don't need to keep time on APs as it is handled on the BSP */
	if (cpuid != bsp_cpu_id)
		stop_local_timer();
	else
#endif
	{
		/*
		 * If the timer has expired while in kernel we must
		 * rearm it before we go to sleep
		 */
		restart_local_timer();
	}

	/* start accounting for the idle time */
	context_stop(proc_addr(KERNEL));
	ktzprofile_event(KTRACE_IDLE_START);
#if !SPROFILE
	halt_cpu();
	//BKL_LOCK();
#else
	if (!sprofiling)
		halt_cpu();
	else {
		volatile int * v;

		v = get_cpulocal_var_ptr(idle_interrupted);
		interrupts_enable();
		while (!*v)
			arch_pause();
		interrupts_disable();
		*v = 0;
	}
#endif
	ktzprofile_event(KTRACE_IDLE_STOP);
	/*
	 * end of accounting for the idle task does not happen here, the kernel
	 * is handling stuff for quite a while before it gets back here!
	 */
}

/*===========================================================================*
 *                              vm_suspend                                *
 *===========================================================================*/
void vm_suspend(struct proc *caller, const struct proc *target,
        const vir_bytes linaddr, const vir_bytes len, const int type,
        const int writeflag)
{
        /* This range is not OK for this process. Set parameters
         * of the request and notify VM about the pending request.
         */
	assert(proc_locked(caller));
	assert(proc_locked(target));
        assert(!RTS_ISSET(caller, RTS_VMREQUEST));
        assert(!RTS_ISSET(target, RTS_VMREQUEST));

        RTS_SET(caller, RTS_VMREQUEST);

        caller->p_vmrequest.req_type = VMPTYPE_CHECK;
        caller->p_vmrequest.target = target->p_endpoint;
        caller->p_vmrequest.params.check.start = linaddr;
        caller->p_vmrequest.params.check.length = len;
        caller->p_vmrequest.params.check.writeflag = writeflag;
        caller->p_vmrequest.type = type;

        /* Connect caller on vmrequest wait queue. */
	lock_vmrequest();
        if(!(caller->p_vmrequest.nextrequestor = vmrequest))
                if(OK != send_sig_deferred(VM_PROC_NR, SIGKMEM))
                        panic("send_sig failed");
        vmrequest = caller;
	unlock_vmrequest();
}

/*===========================================================================*
 *                              delivermsg                                *
 *===========================================================================*/
static void delivermsg(struct proc *rp)
{
        assert(!RTS_ISSET(rp, RTS_VMREQUEST));
        assert(rp->p_misc_flags & MF_DELIVERMSG);
        assert(rp->p_delivermsg.m_source != NONE);

        if (copy_msg_to_user(&rp->p_delivermsg,
                                (message *) rp->p_delivermsg_vir)) {
                if(rp->p_misc_flags & MF_MSGFAILED) {
                        /* 2nd consecutive failure means this won't succeed */
                        printf("WARNING wrong user pointer 0x%08lx from "
                                "process %s / %d\n",
                                rp->p_delivermsg_vir,
                                rp->p_name,
                                rp->p_endpoint);
                        cause_sig_deferred(rp->p_nr, SIGSEGV);
                } else {
                        /* 1st failure means we have to ask VM to handle it */
                        vm_suspend(rp, rp, rp->p_delivermsg_vir,
                                sizeof(message), VMSTYPE_DELIVERMSG, 1);
                        rp->p_misc_flags |= MF_MSGFAILED;
                }
        } else {
                /* Indicate message has been delivered; address is 'used'. */
                rp->p_delivermsg.m_source = NONE;
                rp->p_misc_flags &= ~(MF_DELIVERMSG|MF_MSGFAILED);
		ktzprofile_deliver_msg(&(rp->p_delivermsg));

                if(!(rp->p_misc_flags & MF_CONTEXT_SET)) {
                        rp->p_reg.retreg = OK;
                }
        }
}

/*===========================================================================*
 *				switch_to_user				     * 
 *===========================================================================*/
void switch_to_user(void)
{
	/* This function is called an instant before proc_ptr is
	 * to be scheduled again.
	 */
	struct proc * p;
#ifdef CONFIG_SMP
	int tlb_must_refresh = 0;
#endif

	/* Send all the signals from the kernel operation we just performed. */
	handle_all_deferred_sigs();

	p = get_cpulocal_var(proc_ptr);
	lock_proc(p);

	/*
	 * if the current process is still runnable check the misc flags and let
	 * it run unless it becomes not runnable in the meantime
	 */
	if (proc_is_runnable(p))
		goto check_misc_flags;
	/*
	 * if a process becomes not runnable while handling the misc flags, we
	 * need to pick a new one here and start from scratch. Also if the
	 * current process wasn't runnable, we pick a new one here
	 */
not_runnable_pick_new:
        /* If we end up here after a resumed kernel call or a delivermsg,
         * handle the signals if any. We need to do this before the potential
         * enqueue below, because the proc_ptr is set to `p` at this point.
	 * This explanation sucks, all I know is the deferred sigs must be
	 * sent *before* the potential enqueue, but I don't remember why :/ */
	if(get_cpulocal_var(sigbuffer_count)>0) {
		unlock_proc(p);
		handle_all_deferred_sigs();
		lock_proc(p);
	}

	assert(proc_locked(p));
	if (proc_is_migrating(p)) {
		/* Somebody wants to migrate this process. now that its
		 * time-slice or kernel operation is over we can migrate it. */
		assert(p->p_next_cpu!=-1);
		p->p_cpu = p->p_next_cpu;
		p->p_next_cpu = -1;
		/* Enqueue p on its new cpu. */
		RTS_UNSET(p,RTS_PROC_MIGR);
	} else if (proc_is_preempted(p)) {
		p->p_rts_flags &= ~RTS_PREEMPTED;
		if (proc_is_runnable(p)) {
			if (p->p_cpu_time_left)
				enqueue_head(p);
			else
				enqueue(p);
		}
	}

	/* Set the proc_ptr to the idle proc. That way if we receive a migrate
	 * NMI request after exiting the while loop below, but before changing
	 * proc_ptr, the cpu will not mistakenly use the "old" value of
	 * proc_ptr in smp_sched_handler. */
	get_cpulocal_var(proc_ptr) = get_cpulocal_var_ptr(idle_proc);
	unlock_proc(p);

	/*
	 * if we have no process to run, set IDLE as the current process for
	 * time accounting and put the cpu in an idle state. After the next
	 * timer interrupt the execution resumes here and we can pick another
	 * process. If there is still nothing runnable we "schedule" IDLE again
	 */
retry_pick:
	lock_runqueues(cpuid);
	while (!(p = pick_proc())) {
		/* Set the idle state while holding the queue lock to avoid
		 * race conditions. */
		get_cpulocal_var(cpu_is_idle) = 1;
		unlock_runqueues(cpuid);
		idle();
		/* We might have scheduled some signal when waking up from the
		 * halt. handle them now. */
		handle_all_deferred_sigs();
		lock_runqueues(cpuid);
	}
	unlock_runqueues(cpuid);

	lock_proc(p);
	if(p->p_cpu!=cpuid) {
		/* We have a small race-condition here. During the small gap
		 * between the proc selection and the lock on p, p might have
		 * been migrated. In this case, simply do nothing on p, it is
		 * not owned by this cpu anymore. And retry the pick_proc. */
		unlock_proc(p);
		goto retry_pick;
	}
	if(!proc_is_runnable(p)) {
		goto not_runnable_pick_new;
	}

	/* update the global variable */
	get_cpulocal_var(proc_ptr) = p;

#ifdef CONFIG_SMP
	if (p->p_misc_flags & MF_FLUSH_TLB && get_cpulocal_var(ptproc) == p)
		tlb_must_refresh = 1;
#endif
	switch_address_space(p);

check_misc_flags:

	assert(proc_is_runnable(p));

	/* The tracing capabilities have been disabled to break the BKL more
	 * easily. It shouldn't be a problem for our kind of workloads.
	 * Nonetheless we don't want to have silent errors, and a future
	 * work would be to enable those again. */
	assert(!(p->p_misc_flags&MF_SC_DEFER));
	assert(!(p->p_misc_flags&MF_SC_TRACE));
	assert(!(p->p_misc_flags&MF_SC_ACTIVE));

	while (p->p_misc_flags & (MF_KCALL_RESUME | MF_DELIVERMSG)) {

		assert(proc_is_runnable(p));
		if (p->p_misc_flags & MF_KCALL_RESUME) {
			kernel_call_resume(p);
			lock_proc(p);
		}
		else if (p->p_misc_flags & MF_DELIVERMSG) {
			TRACE(VF_SCHEDULING, printf("delivering to %s / %d\n",
				p->p_name, p->p_endpoint););
			delivermsg(p);
		}

		/*
		 * the selected process might not be runnable anymore. We have
		 * to checkit and schedule another one
		 */
		if (!proc_is_runnable(p)) {
			goto not_runnable_pick_new;
		}
	}

	/*
	 * check the quantum left before it runs again. We must do it only here
	 * as we are sure that a possible out-of-quantum message to the
	 * scheduler will not collide with the regular ipc
	 */
	if (!p->p_cpu_time_left)
		proc_no_time(p);

	if(get_cpulocal_var(sigbuffer_count)>0) {
		unlock_proc(p);
		handle_all_deferred_sigs();
		lock_proc(p);
	}

	if (!proc_is_runnable(p)) {
		goto not_runnable_pick_new;
	} else {
		unlock_proc(p);
	}

	TRACE(VF_SCHEDULING, printf("cpu %d starting %s / %d "
				"pc 0x%08x\n",
		cpuid, p->p_name, p->p_endpoint, p->p_reg.pc););
#if DEBUG_TRACE
	p->p_schedules++;
#endif

	p = arch_finish_switch_to_user();
	assert(p->p_cpu_time_left);

	context_stop(proc_addr(KERNEL));

	/* If the process isn't the owner of FPU, enable the FPU exception */
	if (get_cpulocal_var(fpu_owner) != p)
		enable_fpu_exception();
	else
		disable_fpu_exception();

	/* If MF_CONTEXT_SET is set, don't clobber process state within
	 * the kernel. The next kernel entry is OK again though.
	 */
	p->p_misc_flags &= ~MF_CONTEXT_SET;

#if defined(__i386__)
  	assert(p->p_seg.p_cr3 != 0);
#elif defined(__arm__)
	assert(p->p_seg.p_ttbr != 0);
#endif
#ifdef CONFIG_SMP
	if (p->p_misc_flags & MF_FLUSH_TLB) {
		if (tlb_must_refresh)
			refresh_tlb();
		p->p_misc_flags &= ~MF_FLUSH_TLB;
	}
#endif
	
	restart_local_timer();

	/* We are definitely going to user space now. Notify the profiler. */
	ktzprofile_event(KTRACE_USER_START);

	/* Check that we did not forget to send a signal. */
	assert(get_cpulocal_var(sigbuffer_count)==0);
	
	/*
	 * restore_user_context() carries out the actual mode switch from kernel
	 * to userspace. This function does not return
	 */
	restore_user_context(p);
	NOT_REACHABLE;
}

/*
 * handler for all synchronous IPC calls
 */
static int do_sync_ipc(struct proc * caller_ptr, /* who made the call */
			int call_nr,	/* system call number and flags */
			endpoint_t src_dst_e,	/* src or dst of the call */
			message *m_ptr)	/* users pointer to a message */
{
  int result;					/* the system call's result */
  int src_dst_p;				/* Process slot number */
  char *callname;

  /* Check destination. RECEIVE is the only call that accepts ANY (in addition
   * to a real endpoint). The other calls (SEND, SENDREC, and NOTIFY) require an
   * endpoint to corresponds to a process. In addition, it is necessary to check
   * whether a process is allowed to send to a given destination.
   */
  assert(call_nr != SENDA);

  /* Only allow non-negative call_nr values less than 32 */
  if (call_nr < 0 || call_nr > IPCNO_HIGHEST || call_nr >= 32
      || !(callname = ipc_call_names[call_nr])) {
#if DEBUG_ENABLE_IPC_WARNINGS
      printf("sys_call: trap %d not allowed, caller %d, src_dst %d\n", 
          call_nr, proc_nr(caller_ptr), src_dst_e);
#endif
	return(ETRAPDENIED);		/* trap denied by mask or kernel */
  }

  if (src_dst_e == ANY)
  {
	if (call_nr != RECEIVE)
	{
		return EINVAL;
	}
	src_dst_p = (int) src_dst_e;
  }
  else
  {
	/* Require a valid source and/or destination process. */
	if(!isokendpt(src_dst_e, &src_dst_p)) {
		return EDEADSRCDST;
	}

	/* If the call is to send to a process, i.e., for SEND, SENDNB,
	 * SENDREC or NOTIFY, verify that the caller is allowed to send to
	 * the given destination. 
	 */
	if (call_nr != RECEIVE)
	{
		if (!may_send_to(caller_ptr, src_dst_p)) {
#if DEBUG_ENABLE_IPC_WARNINGS
			printf(
			"sys_call: ipc mask denied %s from %d to %d\n",
				callname,
				caller_ptr->p_endpoint, src_dst_e);
#endif
			return(ECALLDENIED);	/* call denied by ipc mask */
		}
	}
  }

  /* Check if the process has privileges for the requested call. Calls to the 
   * kernel may only be SENDREC, because tasks always reply and may not block 
   * if the caller doesn't do receive(). 
   */
  if (!(priv(caller_ptr)->s_trap_mask & (1 << call_nr))) {
#if DEBUG_ENABLE_IPC_WARNINGS
      printf("sys_call: %s not allowed, caller %d, src_dst %d\n", 
          callname, proc_nr(caller_ptr), src_dst_p);
#endif
	return(ETRAPDENIED);		/* trap denied by mask or kernel */
  }

  if (call_nr != SENDREC && call_nr != RECEIVE && iskerneln(src_dst_p)) {
#if DEBUG_ENABLE_IPC_WARNINGS
      printf("sys_call: trap %s not allowed, caller %d, src_dst %d\n",
           callname, proc_nr(caller_ptr), src_dst_e);
#endif
	return(ETRAPDENIED);		/* trap denied by mask or kernel */
  }

  switch(call_nr) {
  case SENDREC:
	result = mini_sendrec(caller_ptr, src_dst_e, m_ptr, 0);
	break;
  case SEND:			
	result = mini_send(caller_ptr, src_dst_e, m_ptr, 0);
	if (call_nr == SEND || result != OK)
		break;				/* done, or SEND failed */
	/* fall through for SENDREC */
  case RECEIVE:			
	if (call_nr == RECEIVE) {
		caller_ptr->p_misc_flags &= ~MF_REPLY_PEND;
		IPC_STATUS_CLEAR(caller_ptr);  /* clear IPC status code */
	}
	result = mini_receive(caller_ptr, src_dst_e, m_ptr, 0);
	break;
  case NOTIFY:
	result = mini_notify(caller_ptr, src_dst_e);
	break;
  case SENDNB:
        result = mini_send(caller_ptr, src_dst_e, m_ptr, NON_BLOCKING);
        break;
  default:
	result = EBADCALL;			/* illegal system call */
  }

  /* Just making sure. */
  assert(result==OK||(EBADCPU<=result&&result<=ERESTART));

  /* Now, return the result of the system call to the caller. */
  return(result);
}

int do_ipc(reg_t r1, reg_t r2, reg_t r3)
{
  struct proc *const caller_ptr = get_cpulocal_var(proc_ptr);	/* get pointer to caller */
  caller_ptr->p_in_ipc_op = 1;
  int call_nr = (int) r1;
  int res = 0;

  assert(!RTS_ISSET(caller_ptr, RTS_SLOT_FREE));

  /* bill kernel time to this process. */
  get_cpulocal_var(bill_ipc) = caller_ptr;

  /* If this process is subject to system call tracing, handle that first. */
  if (caller_ptr->p_misc_flags & (MF_SC_TRACE | MF_SC_DEFER)) {
	panic("NOT IMPLEMENTED");
  }

  if(caller_ptr->p_misc_flags & MF_DELIVERMSG) {
	panic("sys_call: MF_DELIVERMSG on for %s / %d\n",
		caller_ptr->p_name, caller_ptr->p_endpoint);
  }

  /* Now check if the call is known and try to perform the request. The only
   * system calls that exist in MINIX are sending and receiving messages.
   *   - SENDREC: combines SEND and RECEIVE in a single system call
   *   - SEND:    sender blocks until its message has been delivered
   *   - RECEIVE: receiver blocks until an acceptable message has arrived
   *   - NOTIFY:  asynchronous call; deliver notification or mark pending
   *   - SENDA:   list of asynchronous send requests
   */
  ktzprofile_ipc(call_nr);
  switch(call_nr) {
  	case SENDREC:
  	case SEND:			
  	case RECEIVE:			
  	case NOTIFY:
  	case SENDNB:
  	{
  	    /* Process accounting for scheduling */
	    caller_ptr->p_accounting.ipc_sync++;

	    message *m = (message *)r3;
  	    res = do_sync_ipc(caller_ptr, call_nr, (endpoint_t) r2, m);
	    goto end;
  	}
  	case SENDA:
  	{
 	    /*
  	     * Get and check the size of the argument in bytes as it is a
  	     * table
  	     */
  	    size_t msg_size = (size_t) r2;
  
  	    /* Process accounting for scheduling */
	    caller_ptr->p_accounting.ipc_async++;

	    asynmsg_t *amsg = (asynmsg_t *)r3;
 
  	    /* Limit size to something reasonable. An arbitrary choice is 16
  	     * times the number of process table entries.
  	     */
  	    if (msg_size > 16*(NR_TASKS + NR_PROCS))
			res = EDOM;
	    else
		    res = mini_senda(caller_ptr, amsg, msg_size);
	    goto end;
  	}
  	case MINIX_KERNINFO:
	{
		/* It might not be initialized yet. */
	  	if(!minix_kerninfo_user) {
			res = EBADCALL;
		} else {
			arch_set_secondary_ipc_return(caller_ptr, minix_kerninfo_user);
			res = OK;
		}
		goto end;
	}
  	default:
	{
		res =  EBADCALL;		/* illegal system call */
		goto end;
	}
  }
end:
  /* Indicate end of IPC to the profile. */
  ktzprofile_event(KTRACE_IPC_END);
  caller_ptr->p_in_ipc_op = 0;
  return res;
}

/*===========================================================================*
 *				deadlock				     * 
 *===========================================================================*/
static int deadlock(
  int function,				/* trap number */
  register struct proc *cp,		/* pointer to caller */
  endpoint_t src_dst_e			/* src or dst process */
)
{
/* Check for deadlock. This can happen if 'caller_ptr' and 'src_dst' have
 * a cyclic dependency of blocking send and receive calls. The only cyclic 
 * dependency that is not fatal is if the caller and target directly SEND(REC)
 * and RECEIVE to each other. If a deadlock is found, the group size is 
 * returned. Otherwise zero is returned. 
 */
  register struct proc *xp;			/* process pointer */
  int group_size = 1;				/* start with only caller */
#if DEBUG_ENABLE_IPC_WARNINGS
  static struct proc *processes[NR_PROCS + NR_TASKS];
  processes[0] = cp;
#endif

  while (src_dst_e != ANY) { 			/* check while process nr */
      int src_dst_slot;
      okendpt(src_dst_e, &src_dst_slot);
      xp = proc_addr(src_dst_slot);		/* follow chain of processes */
      assert(proc_ptr_ok(xp));
      assert(!RTS_ISSET(xp, RTS_SLOT_FREE));
#if DEBUG_ENABLE_IPC_WARNINGS
      processes[group_size] = xp;
#endif
      group_size ++;				/* extra process in group */

      /* Check whether the last process in the chain has a dependency. If it 
       * has not, the cycle cannot be closed and we are done.
       */
      if((src_dst_e = P_BLOCKEDON(xp)) == NONE)
	return 0;

      /* Now check if there is a cyclic dependency. For group sizes of two,  
       * a combination of SEND(REC) and RECEIVE is not fatal. Larger groups
       * or other combinations indicate a deadlock.  
       */
      if (src_dst_e == cp->p_endpoint) {	/* possible deadlock */
	  if (group_size == 2) {		/* caller and src_dst */
	      /* The function number is magically converted to flags. */
	      if ((xp->p_rts_flags ^ (function << 2)) & RTS_SENDING) { 
	          return(0);			/* not a deadlock */
	      }
	  }
#if DEBUG_ENABLE_IPC_WARNINGS
	  {
		int i;
		printf("deadlock between these processes:\n");
		for(i = 0; i < group_size; i++) {
			printf(" %10s ", processes[i]->p_name);
		}
		printf("\n\n");
		for(i = 0; i < group_size; i++) {
			print_proc(processes[i]);
			proc_stacktrace(processes[i]);
		}
	  }
#endif
          return(group_size);			/* deadlock found */
      }
  }
  return(0);					/* not a deadlock */
}

/*===========================================================================*
 *				has_pending				     * 
 *===========================================================================*/
static int has_pending(sys_map_t *map, int src_p, int asynm)
{
/* Check to see if there is a pending message from the desired source
 * available.
 */

  int src_id;
  sys_id_t id = NULL_PRIV_ID;
#ifdef CONFIG_SMP
  struct proc * p;
#endif

  /* Either check a specific bit in the mask map, or find the first bit set in
   * it (if any), depending on whether the receive was called on a specific
   * source endpoint.
   */
  if (src_p != ANY) {
	src_id = nr_to_id(src_p);
	if (get_sys_bit(*map, src_id)) {
#ifdef CONFIG_SMP
		p = proc_addr(id_to_nr(src_id));
		if (asynm && RTS_ISSET(p, RTS_VMINHIBIT))
			(void)0;
		else
#endif
			id = src_id;
	}
  } else {
	/* Find a source with a pending message */
	for (src_id = 0; src_id < NR_SYS_PROCS; src_id += BITCHUNK_BITS) {
		if (get_sys_bits(*map, src_id) != 0) {
#ifdef CONFIG_SMP
			while (src_id < NR_SYS_PROCS) {
				while (!get_sys_bit(*map, src_id)) {
					if (src_id == NR_SYS_PROCS)
						goto quit_search;
					src_id++;
				}
				p = proc_addr(id_to_nr(src_id));
				/*
				 * We must not let kernel fiddle with pages of a
				 * process which are currently being changed by
				 * VM.  It is dangerous! So do not report such a
				 * process as having pending async messages.
				 * Skip it.
				 */
				if (asynm && RTS_ISSET(p, RTS_VMINHIBIT)) {
					src_id++;
				} else
					goto quit_search;
			}
#else
			while (!get_sys_bit(*map, src_id)) src_id++;
			goto quit_search;
#endif
		}
	}

quit_search:
	if (src_id < NR_SYS_PROCS)	/* Found one */
		id = src_id;
  }

  return(id);
}

/*===========================================================================*
 *				has_pending_notify			     *
 *===========================================================================*/
int has_pending_notify(struct proc * caller, int src_p)
{
	sys_map_t * map = &priv(caller)->s_notify_pending;
	return has_pending(map, src_p, 0);
}

/*===========================================================================*
 *				has_pending_asend			     *
 *===========================================================================*/
int has_pending_asend(struct proc * caller, int src_p)
{
	sys_map_t * map = &priv(caller)->s_asyn_pending;
	return has_pending(map, src_p, 1);
}

/*===========================================================================*
 *				unset_notify_pending			     *
 *===========================================================================*/
void unset_notify_pending(struct proc * caller, int src_p)
{
	sys_map_t * map = &priv(caller)->s_notify_pending;
	unset_sys_bit(*map, src_p);
}

/*===========================================================================*
 *				mini_send				     * 
 *===========================================================================*/
/* This function assumes that all the required locks are taken before calling
 * it. */
int mini_send_no_lock(
  register struct proc *caller_ptr,	/* who is trying to send a message? */
  endpoint_t dst_e,			/* to whom is message being sent? */
  message *m_ptr,			/* pointer to message buffer */
  const int flags
)
{
/* Send a message from 'caller_ptr' to 'dst'. If 'dst' is blocked waiting
 * for this message, copy the message to it and unblock 'dst'. If 'dst' is
 * not waiting at all, or is waiting for another source, queue 'caller_ptr'.
 */
  register struct proc *dst_ptr;
  register struct proc **xpp;
  int dst_p;
  dst_p = _ENDPOINT_P(dst_e);
  dst_ptr = proc_addr(dst_p);

  assert(proc_locked(caller_ptr));
  assert(proc_locked(dst_ptr));

  if (RTS_ISSET(dst_ptr, RTS_NO_ENDPOINT))
  {
	return EDEADSRCDST;
  }

  /* Check if 'dst' is blocked waiting for this message. The destination's 
   * RTS_SENDING flag may be set when its SENDREC call blocked while sending.  
   */
  if (WILLRECEIVE(caller_ptr->p_endpoint, dst_ptr, (vir_bytes)m_ptr, NULL)) {
	int call;
	/* Destination is indeed waiting for this message. */
	assert(!(dst_ptr->p_misc_flags & MF_DELIVERMSG));	

	if (!(flags & FROM_KERNEL)) {
		if(copy_msg_from_user(m_ptr, &dst_ptr->p_delivermsg))
			return EFAULT;
		if(copy_msg_from_user(m_ptr, &caller_ptr->p_sendmsg))
			return EFAULT;
	} else {
		dst_ptr->p_delivermsg = *m_ptr;
		IPC_STATUS_ADD_FLAGS(dst_ptr, IPC_FLG_MSG_FROM_KERNEL);
	}

	dst_ptr->p_delivermsg.m_source = caller_ptr->p_endpoint;
	dst_ptr->p_misc_flags |= MF_DELIVERMSG;

	call = (caller_ptr->p_misc_flags & MF_REPLY_PEND ? SENDREC
		: (flags & NON_BLOCKING ? SENDNB : SEND));
	IPC_STATUS_ADD_CALL(dst_ptr, call);

	if (dst_ptr->p_misc_flags & MF_REPLY_PEND)
		dst_ptr->p_misc_flags &= ~MF_REPLY_PEND;

	assert(dst_ptr->p_deliver_type==MSG_TYPE_NULL);
	dst_ptr->p_deliver_type = MSG_TYPE_NORMAL;
	RTS_UNSET(dst_ptr, RTS_RECEIVING);

#if DEBUG_IPC_HOOK
	hook_ipc_msgsend(&dst_ptr->p_delivermsg, caller_ptr, dst_ptr);
	hook_ipc_msgrecv(&dst_ptr->p_delivermsg, caller_ptr, dst_ptr);
#endif
  } else {
	if(flags & NON_BLOCKING) {
		return(ENOTREADY);
	}

	/* Check for a possible deadlock before actually blocking. */
	if (deadlock(SEND, caller_ptr, dst_e)) {
		return(ELOCKED);
	}

	/* Destination is not waiting.  Block and dequeue caller. */
	if (!(flags & FROM_KERNEL)) {
		if(copy_msg_from_user(m_ptr, &caller_ptr->p_sendmsg))
			return EFAULT;
	} else {
		caller_ptr->p_sendmsg = *m_ptr;
		/*
		 * we need to remember that this message is from kernel so we
		 * can set the delivery status flags when the message is
		 * actually delivered
		 */
		caller_ptr->p_misc_flags |= MF_SENDING_FROM_KERNEL;
	}

	assert(!RTS_ISSET(caller_ptr,RTS_SENDING));
	RTS_SET(caller_ptr, RTS_SENDING);
	caller_ptr->p_sendto_e = dst_e;

	/* Process is now blocked.  Put in on the destination's queue. */
	assert(caller_ptr->p_q_link == NULL);
	xpp = &dst_ptr->p_caller_q;		/* find end of list */
	while (*xpp) xpp = &(*xpp)->p_q_link;	
	*xpp = caller_ptr;			/* add caller to end */

#if DEBUG_IPC_HOOK
	hook_ipc_msgsend(&caller_ptr->p_sendmsg, caller_ptr, dst_ptr);
#endif
  }
  dst_ptr->p_new_message = 1;
  return(OK);
}

int mini_send(
  register struct proc *caller_ptr,	/* who is trying to send a message? */
  endpoint_t dst_e,			/* to whom is message being sent? */
  message *m_ptr,			/* pointer to message buffer */
  const int flags
)
{
	int dst_p,res;
	struct proc *dst_ptr;

	/* Take all the necessary locks and call mini_send_no_lock. */
	dst_p = _ENDPOINT_P(dst_e);
	dst_ptr = proc_addr(dst_p);

	lock_two_procs(caller_ptr,dst_ptr);
	res = mini_send_no_lock(caller_ptr,dst_e,m_ptr,flags);
	unlock_two_procs(caller_ptr,dst_ptr);

	return res;
}

/*===========================================================================*
 *				mini_sendrec				     * 
 *===========================================================================*/
static int mini_sendrec_no_lock(struct proc *caller_ptr, struct proc *to,
	message *m_buff_usr, int flags)
{
	int other_p,result;
	const int src = to->p_endpoint;
	assert(proc_locked(caller_ptr));
	/* A flag is set so that notifications cannot interrupt SENDREC. */
	caller_ptr->p_misc_flags |= MF_REPLY_PEND;
	result = mini_send_no_lock(caller_ptr, src, m_buff_usr, 0);
	unlock_proc(to);
	if(result==OK) {
		result = mini_receive_no_lock(caller_ptr, src, m_buff_usr, 0);
	}
	return result;
}

static int mini_sendrec(struct proc *caller_ptr, endpoint_t src,
	message *m_buff_usr, int flags)
{
	int other_p,res;
	struct proc *other_ptr;

	other_p = _ENDPOINT_P(src);
	other_ptr = proc_addr(other_p);

	/* We need to take the union of the locks needed for send and 
	 * receive. Which in our case is the caller and the other proc (as the
	 * receive is not ANY. */

	lock_two_procs(caller_ptr,other_ptr);
	res = mini_sendrec_no_lock(caller_ptr,other_ptr,m_buff_usr,flags);

	/* other_ptr has been unlocked in mini_sendrec_no_lock. */
	unlock_proc(caller_ptr);

	return res;
}

/*===========================================================================*
 *				mini_receive				     * 
 *===========================================================================*/
static int set_waiting_receiving(struct proc *caller_ptr, endpoint_t src_e, int non_blocking)
{
  if (non_blocking) {
	return ENOTREADY;
  } else {
      /* Check for a possible deadlock before actually blocking. */
      if (deadlock(RECEIVE, caller_ptr, src_e)) {
          return(ELOCKED);
      }

      caller_ptr->p_getfrom_e = src_e;		
      caller_ptr->p_deliver_type = MSG_TYPE_NULL;
      RTS_SET(caller_ptr, RTS_RECEIVING);
      return(OK);
  }
}

static int check_pending_notif(struct proc *caller_ptr,int src_e,int src_p)
{
	/* Check for pending notifications */
	const int src_id = has_pending_notify(caller_ptr, src_p);
	const int found = src_id != NULL_PRIV_ID;
	int src_proc_nr,sender_e;

	if(found) {
		src_proc_nr = id_to_nr(src_id);		/* get source proc */
		sender_e = proc_addr(src_proc_nr)->p_endpoint;
	}

	if (found&&CANRECEIVE(src_e,sender_e,caller_ptr,0,&m_notify_buff)) {

#if DEBUG_ENABLE_IPC_WARNINGS
		if(src_proc_nr == NONE) {
			printf("mini_receive: sending notify from NONE\n");
		}
#endif
		assert(src_proc_nr != NONE);
		unset_notify_pending(caller_ptr, src_id);	/* no longer pending */

		/* Found a suitable source, deliver the notification message. */
		assert(!(caller_ptr->p_misc_flags & MF_DELIVERMSG));	
		assert(src_e == ANY || sender_e == src_e);

		/* assemble message */
		BuildNotifyMessage(&caller_ptr->p_delivermsg, src_proc_nr, caller_ptr);
		caller_ptr->p_delivermsg.m_source = sender_e;
		caller_ptr->p_misc_flags |= MF_DELIVERMSG;

		IPC_STATUS_ADD_CALL(caller_ptr, NOTIFY);

		return 1; // Success
	}
	return 0; // Failure
}

static int check_pending_async(struct proc *caller_ptr,int src_e,int src_p)
{
	/* Check for pending asynchronous messages */
	int r;
	if (has_pending_asend(caller_ptr, src_p) != NULL_PRIV_ID) {
		if (src_p != ANY) {
			/* We alredy acquired the locks in mini_receive_no_lock. */
			r = try_one(src_e, proc_addr(src_p), caller_ptr);
		} else {
			r = try_async(caller_ptr);
		}

		if (r == OK) {
			IPC_STATUS_ADD_CALL(caller_ptr, SENDA);
			return 1;
		}
	}
	return 0;
}

static int check_caller_queue(struct proc *caller_ptr,int src_e)
{
	int result = 0;
	if(src_e!=ANY) {
		/* If we want to deliver from a particular endpoint, no need
		 * to go over the entire caller list. */
		int src_p;
		okendpt(src_e, &src_p);
		struct proc *const src_ptr = proc_addr(src_p);

		lock_two_procs(caller_ptr,src_ptr);
		if(src_ptr->p_sendto_e==caller_ptr->p_endpoint) {
			/* The source is indeed in the caller chain. */
			assert(CANRECEIVE(src_e, src_e, caller_ptr, 0, &src_ptr->p_sendmsg));
			assert(proc_locked(src_ptr));
			assert(proc_locked(caller_ptr));
			assert(!RTS_ISSET(src_ptr, RTS_SLOT_FREE));
			assert(!RTS_ISSET(src_ptr, RTS_NO_ENDPOINT));

			/* Found acceptable message. Copy it and update status. */
			assert(!(caller_ptr->p_misc_flags & MF_DELIVERMSG));
			caller_ptr->p_delivermsg = src_ptr->p_sendmsg;
			caller_ptr->p_delivermsg.m_source = src_ptr->p_endpoint;
			caller_ptr->p_misc_flags |= MF_DELIVERMSG;
			RTS_UNSET(src_ptr, RTS_SENDING);

			const int call = (src_ptr->p_misc_flags & MF_REPLY_PEND ? SENDREC : SEND);
			IPC_STATUS_ADD_CALL(caller_ptr, call);

			/*
			 * if the message is originally from the kernel on behalf of this
			 * process, we must send the status flags accordingly
			 */
			if (src_ptr->p_misc_flags & MF_SENDING_FROM_KERNEL) {
				IPC_STATUS_ADD_FLAGS(caller_ptr, IPC_FLG_MSG_FROM_KERNEL);
				/* we can clean the flag now, not need anymore */
				src_ptr->p_misc_flags &= ~MF_SENDING_FROM_KERNEL;
			}
			if (src_ptr->p_misc_flags & MF_SIG_DELAY)
				sig_delay_done(src_ptr);

#if DEBUG_IPC_HOOK
			hook_ipc_msgrecv(&caller_ptr->p_delivermsg, *xpp, caller_ptr);
#endif

			/* Remove src_ptr from the caller chain. */
			struct proc **xpp = &caller_ptr->p_caller_q;
			while (*xpp!=src_ptr) {
				/* src_ptr must be in the chain. */
				assert(*xpp);
				assert((*xpp)->p_q_link);
				xpp = &((*xpp)->p_q_link);	
				assert(*xpp);
			}
			*xpp = src_ptr->p_q_link;
			src_ptr->p_q_link = NULL;
			src_ptr->p_sendto_e = NONE; // Reset
			result = 1;
		}
		unlock_two_procs(caller_ptr,src_ptr);
	} else {
		/* If we want to deliver from ANY simply take the first proc
		 * in the caller chain. */
		struct proc *const first = caller_ptr->p_caller_q;
		if(!first) {
			return 0;
		}
		const int first_e = first->p_endpoint;

		lock_two_procs(caller_ptr,first);
		// TODO: The following assert will fail in case of a race cond.
		// Just get rid of the race cond already. But this one might
		// not occur that often (or never).
		assert(caller_ptr->p_caller_q==first);
		assert(CANRECEIVE(src_e, first_e, caller_ptr, 0, &first->p_sendmsg));
		assert(proc_locked(first));
		assert(proc_locked(caller_ptr));
		assert(!RTS_ISSET(first, RTS_SLOT_FREE));
		assert(!RTS_ISSET(first, RTS_NO_ENDPOINT));

		/* Found acceptable message. Copy it and update status. */
		assert(!(caller_ptr->p_misc_flags & MF_DELIVERMSG));
		caller_ptr->p_delivermsg = first->p_sendmsg;
		caller_ptr->p_delivermsg.m_source = first->p_endpoint;
		caller_ptr->p_misc_flags |= MF_DELIVERMSG;
		RTS_UNSET(first, RTS_SENDING);

		const int call = (first->p_misc_flags & MF_REPLY_PEND ? SENDREC : SEND);
		IPC_STATUS_ADD_CALL(caller_ptr, call);

		/*
		 * if the message is originally from the kernel on behalf of this
		 * process, we must send the status flags accordingly
		 */
		if (first->p_misc_flags & MF_SENDING_FROM_KERNEL) {
			IPC_STATUS_ADD_FLAGS(caller_ptr, IPC_FLG_MSG_FROM_KERNEL);
			/* we can clean the flag now, not need anymore */
			first->p_misc_flags &= ~MF_SENDING_FROM_KERNEL;
		}
		if (first->p_misc_flags & MF_SIG_DELAY)
			sig_delay_done(first);

#if DEBUG_IPC_HOOK
		hook_ipc_msgrecv(&caller_ptr->p_delivermsg, *xpp, caller_ptr);
#endif

		caller_ptr->p_caller_q = first->p_q_link;
		first->p_q_link = NULL; // remove from chain
		first->p_sendto_e = NONE;
		unlock_two_procs(caller_ptr,first);
		result = 1;
	}
	return result;
}

static void receive_done(struct proc *caller_ptr)
{
  if (caller_ptr->p_misc_flags & MF_REPLY_PEND)
	  caller_ptr->p_misc_flags &= ~MF_REPLY_PEND;
}

static int mini_receive_no_lock(struct proc * caller_ptr,
			endpoint_t src_e, /* which message source is wanted */
			message * m_buff_usr, /* pointer to message buffer */
			const int flags)
{
/* A process or task wants to get a message.  If a message is already queued,
 * acquire it and deblock the sender.  If no message from the desired source
 * is available block the caller.
 */
  register struct proc **xpp;
  struct proc *src_ptr;
  int r, src_id, found, src_proc_nr, src_p;
  endpoint_t sender_e;
  const int is_non_blocking = flags&NON_BLOCKING;
  int tries = 0;

retry:
  tries ++;
  assert(proc_locked(caller_ptr));
  assert(!(caller_ptr->p_misc_flags & MF_DELIVERMSG));

  /* This is where we want our message. */
  caller_ptr->p_delivermsg_vir = (vir_bytes) m_buff_usr;

  get_cpulocal_var(n_receive)++;

  if(src_e == ANY) {
	  src_p = ANY;
	  get_cpulocal_var(n_receive_any)++;
  } else
  {
	okendpt(src_e, &src_p);
	if (RTS_ISSET(proc_addr(src_p), RTS_NO_ENDPOINT))
	{
		return EDEADSRCDST;
	}
  }

  /* Check to see if a message from desired source is already available.  The
   * caller's RTS_SENDING flag may be set if SENDREC couldn't send. If it is
   * set, the process should be blocked.
   */
  if(RTS_ISSET(caller_ptr,RTS_SENDING)) {
	  return set_waiting_receiving(caller_ptr,src_e,is_non_blocking);
  }

  /* Check if there are pending notifications, except for SENDREC. */
  if (! (caller_ptr->p_misc_flags & MF_REPLY_PEND)) {
	  /* We don't need any other lock for notifs. */
	  if(check_pending_notif(caller_ptr,src_e,src_p)) {
		  receive_done(caller_ptr);
		  return OK;
	  }
  }

  /* Checking the async messages and the caller queue will need other locks.
   * Which means we will have to release caller_ptr at some point.
   * By doing so another proc may send us a message in the mean time, look at
   * p_new_message to check if it happened. */
  caller_ptr->p_new_message = 0;
  unlock_proc(caller_ptr);

  if(src_p!=ANY) {
	  /* In case of non-ANY source we can already acquire the locks. */
	  src_ptr = proc_addr(src_p);
	  lock_two_procs(caller_ptr,src_ptr);
  }
  r = check_pending_async(caller_ptr,src_e,src_p);
  if(src_p!=ANY) {
	  unlock_two_procs(caller_ptr,src_ptr);
  }
  if(r) {
	  /* We found an async message, deliver it. */
	  lock_proc(caller_ptr); // The caller expects it.
	  receive_done(caller_ptr);
	  return OK;
  }

  /* Finally check the caller queue. */
  if(check_caller_queue(caller_ptr,src_e)) {
	  lock_proc(caller_ptr); // The caller expects it.
	  receive_done(caller_ptr);
	  return OK;
  }

  /* Nothing worked, check if nobody sent a message in the meantime. If not
   * then we can safely block. */
  lock_proc(caller_ptr);
  if(caller_ptr->p_new_message)
	  goto retry;
  else
	  return set_waiting_receiving(caller_ptr,src_e,is_non_blocking);
}

/*
 * Return the next pending notif. Should be called with lock on caller_ptr.
 */
static struct proc *peek_pending_notif(struct proc *caller_ptr)
{
        int src_id = has_pending_notify(caller_ptr,ANY);
        if(src_id!=NULL_PRIV_ID) {
		return proc_addr(id_to_nr(src_id));
        } else {
		return NULL;
	}
}

static struct proc *peek_pending_async(struct proc *caller_ptr)
{
	/* TODO: This is a copy-pasta from try_async, needs to be cleaned. */
	int r;
	struct priv *privp;
	struct proc *src_ptr;
	sys_map_t *map;

	map = &priv(caller_ptr)->s_asyn_pending;

	/* Try all privilege structures */
	for (privp = BEG_PRIV_ADDR; privp < END_PRIV_ADDR; ++privp)  {
		if (privp->s_proc_nr == NONE)
			continue;

		if (!get_sys_bit(*map, privp->s_id)) 
			continue;

		src_ptr = proc_addr(privp->s_proc_nr);

#ifdef CONFIG_SMP
		/*
		 * Do not copy from a process which does not have a stable address space
		 * due to VM fiddling with it
		 */
		//TODO: Should we lock src_ptr ?
		if (RTS_ISSET(src_ptr, RTS_VMINHIBIT)) {
			src_ptr->p_misc_flags |= MF_SENDA_VM_MISS;
			continue;
		}
#endif
		return src_ptr;
	}
	return NULL;
}

static struct proc *peek_queue(struct proc *caller_ptr)
{
    return caller_ptr->p_caller_q;
}

static int mini_receive(struct proc * caller_ptr,
			endpoint_t src_e, /* which message source is wanted */
			message * m_buff_usr, /* pointer to message buffer */
			const int flags)
{
	lock_proc(caller_ptr);
	const int res = mini_receive_no_lock(caller_ptr,src_e,m_buff_usr,flags);
	unlock_proc(caller_ptr);
	return res;
}

/*===========================================================================*
 *				mini_notify				     * 
 *===========================================================================*/
int mini_notify_no_lock(
  struct proc *caller_ptr,	/* sender of the notification */
  endpoint_t dst_e			/* which process to notify */
)
{
  register struct proc *dst_ptr;
  int src_id;				/* source id for late delivery */
  int dst_p;

  if (!isokendpt(dst_e, &dst_p)) {
	util_stacktrace();
	printf("mini_notify: bogus endpoint %d\n", dst_e);
	return EDEADSRCDST;
  }

  dst_ptr = proc_addr(dst_p);
  dst_ptr->p_new_message = 1;

  assert(proc_locked(dst_ptr));

  /* Check to see if target is blocked waiting for this message. A process 
   * can be both sending and receiving during a SENDREC system call.
   */
  if (WILLRECEIVE(caller_ptr->p_endpoint, dst_ptr, 0, &m_notify_buff) &&
    !(dst_ptr->p_misc_flags & MF_REPLY_PEND)) {
      /* Destination is indeed waiting for a message. Assemble a notification 
       * message and deliver it. Copy from pseudo-source HARDWARE, since the
       * message is in the kernel's address space.
       */
      assert(!(dst_ptr->p_misc_flags & MF_DELIVERMSG));

      BuildNotifyMessage(&dst_ptr->p_delivermsg, proc_nr(caller_ptr), dst_ptr);
      dst_ptr->p_delivermsg.m_source = caller_ptr->p_endpoint;
      dst_ptr->p_misc_flags |= MF_DELIVERMSG;

	assert(dst_ptr->p_deliver_type==MSG_TYPE_NULL);
	dst_ptr->p_deliver_type = MSG_TYPE_NOTIFY;

      IPC_STATUS_ADD_CALL(dst_ptr, NOTIFY);
      RTS_UNSET(dst_ptr, RTS_RECEIVING);

      return(OK);
  } 

  /* Destination is not ready to receive the notification. Add it to the 
   * bit map with pending notifications. Note the indirectness: the privilege id
   * instead of the process number is used in the pending bit map.
   */ 
  src_id = priv(caller_ptr)->s_id;
  set_sys_bit(priv(dst_ptr)->s_notify_pending, src_id); 
  return(OK);
}

int mini_notify(
  struct proc *caller_ptr,	/* sender of the notification */
  endpoint_t dst_e			/* which process to notify */
)
{
	int dst_p,res;
	struct proc *dst_ptr;

	if (!isokendpt(dst_e, &dst_p)) {
		panic("");
	}

	dst_ptr = proc_addr(dst_p);

	lock_two_procs(caller_ptr,dst_ptr);
	res = mini_notify_no_lock(caller_ptr,dst_e);
	unlock_two_procs(caller_ptr,dst_ptr);

	return res;
}

#define ASCOMPLAIN(caller, entry, field)	\
	printf("kernel:%s:%d: asyn failed for %s in %s "	\
	"(%d/%zu, tab 0x%lx)\n",__FILE__,__LINE__,	\
field, caller->p_name, entry, priv(caller)->s_asynsize, priv(caller)->s_asyntab)

#define A_RETR(entry) do {			\
  if (data_copy(				\
  		caller_ptr->p_endpoint, table_v + (entry)*sizeof(asynmsg_t),\
  		KERNEL, (vir_bytes) &tabent,	\
  		sizeof(tabent)) != OK) {	\
  			ASCOMPLAIN(caller_ptr, entry, "message entry");	\
  			r = EFAULT;		\
	                goto asyn_error; \
  }						\
  else if(tabent.dst == SELF) { \
      tabent.dst = caller_ptr->p_endpoint; \
  } \
  			 } while(0)

#define A_INSRT(entry) do {			\
  if (data_copy(KERNEL, (vir_bytes) &tabent,	\
  		caller_ptr->p_endpoint, table_v + (entry)*sizeof(asynmsg_t),\
  		sizeof(tabent)) != OK) {	\
  			ASCOMPLAIN(caller_ptr, entry, "message entry");	\
			/* Do NOT set r or goto asyn_error here! */ \
  }						\
  			  } while(0)	

/*===========================================================================*
 *				try_deliver_senda			     *
 *===========================================================================*/
int try_deliver_senda(struct proc *caller_ptr,
				asynmsg_t *table,
				size_t size,
				int lock)
{
  int r, dst_p, done, do_notify;
  unsigned int i;
  unsigned flags;
  endpoint_t dst;
  struct proc *dst_ptr;
  struct priv *privp;
  asynmsg_t tabent;
  const vir_bytes table_v = (vir_bytes) table;
  message *m_ptr = NULL;

  assert(proc_locked(caller_ptr));

  privp = priv(caller_ptr);

  privp->s_asynendpoint = caller_ptr->p_endpoint;

  if (size == 0) return(OK);  /* Nothing to do, just return */

  /* Scan the table */
  do_notify = FALSE;
  done = TRUE;

  /* Limit size to something reasonable. An arbitrary choice is 16
   * times the number of process table entries.
   *
   * (this check has been duplicated in sys_call but is left here
   * as a sanity check)
   */
  if (size > 16*(NR_TASKS + NR_PROCS)) {
    r = EDOM;
    return r;
  }

  for (i = 0; i < size; i++) {
	/* Process each entry in the table and store the result in the table.
	 * If we're done handling a message, copy the result to the sender. */
	assert(proc_locked(caller_ptr));

	dst = NONE;
	/* Copy message to kernel */
retry:
	A_RETR(i);
	flags = tabent.flags;
	dst = tabent.dst;

	if (isokendpt(tabent.dst, &dst_p))
		dst_ptr = proc_addr(dst_p);
	else
		dst_ptr = NULL;

	/* Here's some explaination on the trickery that will follow.
	 * Here we are trying to deliver a message from caller_ptr to dst_ptr.
	 * Because we need the locks on both, we have to re-acquire the lock
	 * on the caller to ensure the lock ordering.
	 * However in the meantime, dst_ptr might be running try_one with the
	 * caller_ptr as the source, which mean we can have a race condition on
	 * the message i.
	 * To avoid this, retreive the message i again after re-acquiring the
	 * locks and check that the flags haven't changed in the meantime.
	 * If they did then it means that the try_one beat us and thus retry.
	 */
	if(lock) {
		unlock_proc(caller_ptr);
		lock_two_procs(caller_ptr,dst_ptr);
	}

	A_RETR(i);
	if(tabent.flags!=flags) {
		/* Some one beat us to it, retry. */
		if(lock)
			unlock_proc(dst_ptr);
		goto retry;
	}

	if (flags == 0) {
		if(lock)
			unlock_proc(dst_ptr);
		continue;
	}

	/* 'flags' field must contain only valid bits */
	if(flags & ~(AMF_VALID|AMF_DONE|AMF_NOTIFY|AMF_NOREPLY|AMF_NOTIFY_ERR)) {
		r = EINVAL;
		goto asyn_error;
	}
	if (!(flags & AMF_VALID)) { /* Must contain message */
		r = EINVAL;
		goto asyn_error;
	}
	if (flags & AMF_DONE) {
		if(lock)
			unlock_proc(dst_ptr);
		continue;
	}

	r = OK;
	if (!isokendpt(tabent.dst, &dst_p)) 
		r = EDEADSRCDST; /* Bad destination, report the error */
	else if (iskerneln(dst_p)) 
		r = ECALLDENIED; /* Asyn sends to the kernel are not allowed */
	else if (!may_asynsend_to(caller_ptr, dst_p))
		r = ECALLDENIED; /* Send denied by IPC mask */
	else 	/* r == OK */
		dst_ptr = proc_addr(dst_p);

	/* XXX: RTS_NO_ENDPOINT should be removed */
	if (r == OK && RTS_ISSET(dst_ptr, RTS_NO_ENDPOINT)) {
		r = EDEADSRCDST;
	}
	assert(proc_locked(dst_ptr));

	/* Check if 'dst' is blocked waiting for this message.
	 * If AMF_NOREPLY is set, do not satisfy the receiving part of
	 * a SENDREC.
	 */
	if(r==OK)
		dst_ptr->p_new_message = 1;
	if (r == OK && WILLRECEIVE(caller_ptr->p_endpoint, dst_ptr,
	    (vir_bytes)&table[i].msg, NULL) &&
	    (!(flags&AMF_NOREPLY) || !(dst_ptr->p_misc_flags&MF_REPLY_PEND))) {
		/* Destination is indeed waiting for this message. */
		dst_ptr->p_delivermsg = tabent.msg;
		dst_ptr->p_delivermsg.m_source = caller_ptr->p_endpoint;
		dst_ptr->p_misc_flags |= MF_DELIVERMSG;
		IPC_STATUS_ADD_CALL(dst_ptr, SENDA);
		assert(dst_ptr->p_deliver_type==MSG_TYPE_NULL);
		dst_ptr->p_deliver_type = MSG_TYPE_ASYNC;
		RTS_UNSET(dst_ptr, RTS_RECEIVING);
#if DEBUG_IPC_HOOK
		hook_ipc_msgrecv(&dst_ptr->p_delivermsg, caller_ptr, dst_ptr);
#endif
		/* Keep the lock on caller_ptr for the end of ite (+ the next
		 * one). */
		if(lock)
			unlock_proc(dst_ptr);
	} else if (r == OK) {
		/* Inform receiver that something is pending */
		set_sys_bit(priv(dst_ptr)->s_asyn_pending, 
			    priv(caller_ptr)->s_id); 
		done = FALSE;
		/* Keep the lock on caller_ptr for the end of ite (+ the next
		 * one). */
		if(lock)
			unlock_proc(dst_ptr);
		continue;
	} 

	/* Store results */
	tabent.result = r;
	tabent.flags = flags | AMF_DONE;
	if (flags & AMF_NOTIFY)
		do_notify = TRUE;
	else if (r != OK && (flags & AMF_NOTIFY_ERR))
		do_notify = TRUE;
	A_INSRT(i);	/* Copy results to caller; ignore errors */
	continue;

asyn_error:
	if (dst != NONE)
		printf("KERNEL senda error %d to %d\n", r, dst);
	else
		printf("KERNEL senda error %d\n", r);
  }
  assert(proc_locked(caller_ptr));

  if (do_notify) 
	mini_notify_no_lock(proc_addr(ASYNCM), caller_ptr->p_endpoint);

  if (!done) {
	privp->s_asyntab = (vir_bytes) table;
	privp->s_asynsize = size;
  } else {
	  privp->s_asyntab = -1;
	  privp->s_asynsize = 0;
  }

  return(OK);
}

/*===========================================================================*
 *				mini_senda				     *
 *===========================================================================*/
static int mini_senda_no_lock(struct proc *caller_ptr, asynmsg_t *table, size_t size)
{
  struct priv *privp;
  int res;

  privp = priv(caller_ptr);
  if (!(privp->s_flags & SYS_PROC)) {
	printf( "mini_senda: warning caller has no privilege structure\n");
	return (EPERM);
  }
  return try_deliver_senda(caller_ptr, table, size, 1);
}

static int mini_senda(struct proc *caller_ptr, asynmsg_t *table, size_t size)
{
	int res;

	lock_proc(caller_ptr);
	res = mini_senda_no_lock(caller_ptr,table,size);
	unlock_proc(caller_ptr);

	return res;
}


/*===========================================================================*
 *				try_async				     * 
 *===========================================================================*/
static int try_async(struct proc * caller_ptr)
{
  int r;
  struct priv *privp;
  struct proc *src_ptr;
  sys_map_t *map;

  map = &priv(caller_ptr)->s_asyn_pending;

  /* Try all privilege structures */
  for (privp = BEG_PRIV_ADDR; privp < END_PRIV_ADDR; ++privp)  {
	if (privp->s_proc_nr == NONE)
		continue;

	if (!get_sys_bit(*map, privp->s_id)) 
		continue;

	src_ptr = proc_addr(privp->s_proc_nr);

#ifdef CONFIG_SMP
	/*
	 * Do not copy from a process which does not have a stable address space
	 * due to VM fiddling with it
	 */
	lock_two_procs(caller_ptr,src_ptr);
	if (RTS_ISSET(src_ptr, RTS_VMINHIBIT)) {
		unlock_two_procs(caller_ptr,src_ptr);
		continue;
	}
#endif
	assert(!(caller_ptr->p_misc_flags & MF_DELIVERMSG));
	assert(!RTS_ISSET(src_ptr, RTS_VMINHIBIT));
	r = try_one(ANY, src_ptr, caller_ptr);
	unlock_two_procs(caller_ptr,src_ptr);
	if (r == OK)
		return(r);
  }

  return(ESRCH);
}


/*===========================================================================*
 *				try_one					     *
 *===========================================================================*/
static int try_one(endpoint_t receive_e, struct proc *src_ptr,
    struct proc *dst_ptr)
{
/* Try to receive an asynchronous message from 'src_ptr' */
  int r = EAGAIN, done, do_notify;
  unsigned int flags, i;
  size_t size;
  endpoint_t dst, src_e;
  struct proc *caller_ptr;
  struct priv *privp;
  asynmsg_t tabent;
  vir_bytes table_v;

  assert(proc_locked(src_ptr));
  assert(proc_locked(dst_ptr));

  privp = priv(src_ptr);
  if (!(privp->s_flags & SYS_PROC)) return(EPERM);
  size = privp->s_asynsize;
  table_v = privp->s_asyntab;

  /* Clear table pending message flag. We're done unless we're not. */
  unset_sys_bit(priv(dst_ptr)->s_asyn_pending, privp->s_id);

  if (size == 0) return(EAGAIN);
  if (privp->s_asynendpoint != src_ptr->p_endpoint) return EAGAIN;
  if (!may_asynsend_to(src_ptr, proc_nr(dst_ptr))) return (ECALLDENIED);

  caller_ptr = src_ptr;	/* Needed for A_ macros later on */
  src_e = src_ptr->p_endpoint;

  /* Scan the table */
  do_notify = FALSE;
  done = TRUE;

  for (i = 0; i < size; i++) {
  	/* Process each entry in the table and store the result in the table.
  	 * If we're done handling a message, copy the result to the sender.
  	 * Some checks done in mini_senda are duplicated here, as the sender
  	 * could've altered the contents of the table in the meantime.
  	 */

	/* Copy message to kernel */
	A_RETR(i);
	flags = tabent.flags;
	dst = tabent.dst;

	if (flags == 0) continue;	/* Skip empty entries */

	/* 'flags' field must contain only valid bits */
	if(flags & ~(AMF_VALID|AMF_DONE|AMF_NOTIFY|AMF_NOREPLY|AMF_NOTIFY_ERR))
		r = EINVAL;
	else if (!(flags & AMF_VALID)) /* Must contain message */
		r = EINVAL; 
	else if (flags & AMF_DONE) continue; /* Already done processing */

	/* Clear done flag. The sender is done sending when all messages in the
	 * table are marked done or empty. However, we will know that only
	 * the next time we enter this function or when the sender decides to
	 * send additional asynchronous messages and manages to deliver them
	 * all.
	 */
	done = FALSE;

	if (r == EINVAL)
		goto store_result;

	/* Message must be directed at receiving end */
	if (dst != dst_ptr->p_endpoint) continue;

	if (!CANRECEIVE(receive_e, src_e, dst_ptr,
		table_v + i*sizeof(asynmsg_t) + offsetof(struct asynmsg,msg),
		NULL)) {
		continue;
	}

	/* If AMF_NOREPLY is set, then this message is not a reply to a
	 * SENDREC and thus should not satisfy the receiving part of the
	 * SENDREC. This message is to be delivered later.
	 */
	if ((flags & AMF_NOREPLY) && (dst_ptr->p_misc_flags & MF_REPLY_PEND)) 
		continue;

	/* Destination is ready to receive the message; deliver it */
	r = OK;
	dst_ptr->p_delivermsg = tabent.msg;
	dst_ptr->p_delivermsg.m_source = src_ptr->p_endpoint;
	dst_ptr->p_misc_flags |= MF_DELIVERMSG;
#if DEBUG_IPC_HOOK
	hook_ipc_msgrecv(&dst_ptr->p_delivermsg, src_ptr, dst_ptr);
#endif

store_result:
	/* Store results for sender. We may just have started delivering a
	 * message, so we must not return an error to the caller in the case
	 * that storing the results triggers an error!
	 */
	tabent.result = r;
	tabent.flags = flags | AMF_DONE;
	if (flags & AMF_NOTIFY) do_notify = TRUE;
	else if (r != OK && (flags & AMF_NOTIFY_ERR)) do_notify = TRUE;
	A_INSRT(i);	/* Copy results to sender; ignore errors */

	break;
  }

  /* try_one is only called by mini_received or try_async (which is only called
   * by mini_received. Thus we have the BKL. */
  if (do_notify) 
	mini_notify_no_lock(proc_addr(ASYNCM), src_ptr->p_endpoint);

  if (done) {
	privp->s_asyntab = -1;
	privp->s_asynsize = 0;
  } else {
	assert(proc_locked(dst_ptr));
	set_sys_bit(priv(dst_ptr)->s_asyn_pending, privp->s_id);
  }

asyn_error:
  return(r);
}

/*===========================================================================*
 *				cancel_async				     *
 *===========================================================================*/
int cancel_async(struct proc *src_ptr, struct proc *dst_ptr)
{
/* Cancel asynchronous messages from src to dst, because dst is not interested
 * in them (e.g., dst has been restarted) */
  int done, do_notify;
  unsigned int flags, i;
  size_t size;
  endpoint_t dst;
  struct proc *caller_ptr;
  struct priv *privp;
  asynmsg_t tabent;
  vir_bytes table_v;

  assert(proc_locked(src_ptr));
  assert(proc_locked(dst_ptr));

  privp = priv(src_ptr);
  if (!(privp->s_flags & SYS_PROC)) return(EPERM);
  size = privp->s_asynsize;
  table_v = privp->s_asyntab;

  /* Clear table pending message flag. We're done unless we're not. */
  privp->s_asyntab = -1;
  privp->s_asynsize = 0;
  unset_sys_bit(priv(dst_ptr)->s_asyn_pending, privp->s_id);

  if (size == 0) return(EAGAIN);
  if (!may_send_to(src_ptr, proc_nr(dst_ptr))) return(ECALLDENIED);

  caller_ptr = src_ptr;	/* Needed for A_ macros later on */

  /* Scan the table */
  do_notify = FALSE;
  done = TRUE;


  for (i = 0; i < size; i++) {
  	/* Process each entry in the table and store the result in the table.
  	 * If we're done handling a message, copy the result to the sender.
  	 * Some checks done in mini_senda are duplicated here, as the sender
  	 * could've altered the contents of the table in the mean time.
  	 */

  	int r = EDEADSRCDST;	/* Cancel delivery due to dead dst */

	/* Copy message to kernel */
	A_RETR(i);
	flags = tabent.flags;
	dst = tabent.dst;

	if (flags == 0) continue;	/* Skip empty entries */

	/* 'flags' field must contain only valid bits */
	if(flags & ~(AMF_VALID|AMF_DONE|AMF_NOTIFY|AMF_NOREPLY|AMF_NOTIFY_ERR))
		r = EINVAL;
	else if (!(flags & AMF_VALID)) /* Must contain message */
		r = EINVAL; 
	else if (flags & AMF_DONE) continue; /* Already done processing */

	/* Message must be directed at receiving end */
	if (dst != dst_ptr->p_endpoint) {
		done = FALSE;
		continue;
	}

	/* Store results for sender */
	tabent.result = r;
	tabent.flags = flags | AMF_DONE;
	if (flags & AMF_NOTIFY) do_notify = TRUE;
	else if (r != OK && (flags & AMF_NOTIFY_ERR)) do_notify = TRUE;
	A_INSRT(i);	/* Copy results to sender; ignore errors */
  }

  /* Called by clear_ipc_ref called by kernel_call thus BKL. */
  if (do_notify) 
	mini_notify_no_lock(proc_addr(ASYNCM), src_ptr->p_endpoint);

  if (!done) {
	privp->s_asyntab = table_v;
	privp->s_asynsize = size;
  }

asyn_error:
  return(OK);
}

/*===========================================================================*
 *				enqueue					     * 
 *===========================================================================*/
void enqueue(
  register struct proc *rp	/* this process is now runnable */
)
{
/* Add 'rp' to one of the queues of runnable processes.  This function is 
 * responsible for inserting a process into one of the scheduling queues. 
 * The mechanism is implemented here.   The actual scheduling policy is
 * defined in sched() and pick_proc().
 *
 * This function can be used x-cpu as it always uses the queues of the cpu the
 * process is assigned to.
 */
  int q = rp->p_priority;	 		/* scheduling queue to use */
  int wake_remote_cpu = 0;
  struct proc **rdy_head, **rdy_tail;
  
  assert(proc_is_runnable(rp));

  if(cpuid!=rp->p_cpu)
	  n_remote_enq++;

  assert(q >= 0);

  lock_runqueues(rp->p_cpu);

  rdy_head = get_cpu_var(rp->p_cpu, run_q_head);
  rdy_tail = get_cpu_var(rp->p_cpu, run_q_tail);

  /* Now add the process to the queue. */
  if (!rdy_head[q]) {		/* add to empty queue */
      rdy_head[q] = rdy_tail[q] = rp; 		/* create a new queue */
      rp->p_nextready = NULL;		/* mark new end */
  } 
  else {					/* add to tail of queue */
      rdy_tail[q]->p_nextready = rp;		/* chain tail of queue */	
      rdy_tail[q] = rp;				/* set new queue tail */
      rp->p_nextready = NULL;		/* mark new end */
  }

  /* Check now if we will need to send an IPI to wake the remote cpu.
   * We need to do this while holding the queue lock of the other cpu to
   * avoid race conditions. */
  wake_remote_cpu = (rp->p_cpu!=cpuid)&&get_cpu_var(rp->p_cpu,cpu_is_idle);
  unlock_runqueues(rp->p_cpu);

  rp->p_enqueued = 1;

  if (cpuid == rp->p_cpu) {
	  /*
	   * enqueueing a process with a higher priority than the current one,
	   * it gets preempted. The current process must be preemptible. Testing
	   * the priority also makes sure that a process does not preempt itself
	   */
#if 0
	  /* TODO: Preemption is disabled until we find a way to do it without
	   * race condition (and also remotely).
	   *
	   * ##################################################################
	   * ##################################################################
	   */

	  struct proc * p;
	  p = get_cpulocal_var(proc_ptr);
	  assert(p);
	  if((p->p_priority > rp->p_priority) &&
			  (priv(p)->s_flags & PREEMPTIBLE))
		  RTS_SET_UNSAFE(p, RTS_PREEMPTED); /* calls dequeue() */
#endif
  }
#ifdef CONFIG_SMP
  /*
   * if the process was enqueued on a different cpu and the cpu is idle, i.e.
   * the time is off, we need to wake up that cpu and let it schedule this new
   * process
   */
  else if (wake_remote_cpu) {
	  smp_schedule(rp->p_cpu);
  }
#endif

  /* Make note of when this process was added to queue */
  read_tsc_64(&(get_cpulocal_var(proc_ptr)->p_accounting.enter_queue));


#if DEBUG_SANITYCHECKS
  assert(runqueues_ok_local());
#endif
}

/*===========================================================================*
 *				enqueue_head				     *
 *===========================================================================*/
/*
 * put a process at the front of its run queue. It comes handy when a process is
 * preempted and removed from run queue to not to have a currently not-runnable
 * process on a run queue. We have to put this process back at the fron to be
 * fair
 */
static void enqueue_head(struct proc *rp)
{
  const int q = rp->p_priority;	 		/* scheduling queue to use */

  struct proc **rdy_head, **rdy_tail;

  assert(proc_ptr_ok(rp));
  assert(proc_is_runnable(rp));

  if(cpuid!=rp->p_cpu)
	  n_remote_enq++;

  /*
   * the process was runnable without its quantum expired when dequeued. A
   * process with no time left should have been handled else and differently
   */
  assert(rp->p_cpu_time_left);

  assert(q >= 0);

  lock_runqueues(rp->p_cpu);

  rdy_head = get_cpu_var(rp->p_cpu, run_q_head);
  rdy_tail = get_cpu_var(rp->p_cpu, run_q_tail);

  /* Now add the process to the queue. */
  if (!rdy_head[q]) {		/* add to empty queue */
	rdy_head[q] = rdy_tail[q] = rp; 	/* create a new queue */
	rp->p_nextready = NULL;			/* mark new end */
  } else {					/* add to head of queue */
	rp->p_nextready = rdy_head[q];		/* chain head of queue */
	rdy_head[q] = rp;			/* set new queue head */
  }

  unlock_runqueues(rp->p_cpu);

  rp->p_enqueued = 1;

  /* Make note of when this process was added to queue */
  read_tsc_64(&(get_cpulocal_var(proc_ptr->p_accounting.enter_queue)));


  /* Process accounting for scheduling */
  rp->p_accounting.dequeues--;
  rp->p_accounting.preempted++;

#if DEBUG_SANITYCHECKS
  assert(runqueues_ok_local());
#endif
}

/*===========================================================================*
 *				dequeue					     * 
 *===========================================================================*/
void dequeue(struct proc *rp)
/* this process is no longer runnable */
{
/* A process must be removed from the scheduling queues, for example, because
 * it has blocked.  If the currently active process is removed, a new process
 * is picked to run by calling pick_proc().
 *
 * This function can operate x-cpu as it always removes the process from the
 * queue of the cpu the process is currently assigned to.
 */
  int q = rp->p_priority;		/* queue to use */
  struct proc **xpp;			/* iterate over queue */
  struct proc *prev_xp;
  u64_t tsc, tsc_delta;

  struct proc **rdy_tail;

  assert(proc_ptr_ok(rp));
  assert(!proc_is_runnable(rp));

  /* We don't allow remote dequeues anymore. Use IPI instead. */
  assert(cpuid==rp->p_cpu);
  assert(rp->p_enqueued);

  /* Side-effect for kernel: check if the task's stack still is ok? */
  assert (!iskernelp(rp) || *priv(rp)->s_stack_guard == STACK_GUARD);

  lock_runqueues(rp->p_cpu);
  rdy_tail = get_cpu_var(rp->p_cpu, run_q_tail);

  /* Now make sure that the process is not in its ready queue. Remove the 
   * process if it is found. A process can be made unready even if it is not 
   * running by being sent a signal that kills it.
   */
  prev_xp = NULL;				
  int found = 0;
  for (xpp = get_cpu_var_ptr(rp->p_cpu, run_q_head[q]); *xpp;
		  xpp = &(*xpp)->p_nextready) {
      if (*xpp == rp) {				/* found process to remove */
          *xpp = (*xpp)->p_nextready;		/* replace with next chain */
          if (rp == rdy_tail[q]) {		/* queue tail removed */
              rdy_tail[q] = prev_xp;		/* set new tail */
	  }
	  found = 1;
          break;
      }
      prev_xp = *xpp;				/* save previous in chain */
  }
  unlock_runqueues(rp->p_cpu);
  assert(found);

  rp->p_enqueued = 0;

	
  /* Process accounting for scheduling */
  rp->p_accounting.dequeues++;

  /* this is not all that accurate on virtual machines, especially with
     IO bound processes that only spend a short amount of time in the queue
     at a time. */
  if (rp->p_accounting.enter_queue) {
	read_tsc_64(&tsc);
	tsc_delta = tsc - rp->p_accounting.enter_queue;
	rp->p_accounting.time_in_queue = rp->p_accounting.time_in_queue +
		tsc_delta;
	rp->p_accounting.enter_queue = 0;
  }

  /* For ps(1), remember when the process was last dequeued. */
  rp->p_dequeued = get_monotonic();

#if DEBUG_SANITYCHECKS
  assert(runqueues_ok_local());
#endif
}

/*===========================================================================*
 *				pick_proc				     * 
 *===========================================================================*/
static struct proc * pick_proc(void)
{
/* Decide who to run now.  A new process is selected an returned.
 * When a billable process is selected, record it in 'bill_ptr', so that the 
 * clock task can tell who to bill for system time.
 *
 * This function always uses the run queues of the local cpu!
 */
  register struct proc *rp;			/* process to run */
  struct proc **rdy_head;
  int q;				/* iterate over queues */

  /* Check each of the scheduling queues for ready processes. The number of
   * queues is defined in proc.h, and priorities are set in the task table.
   * If there are no processes ready to run, return NULL.
   */
  rdy_head = get_cpulocal_var(run_q_head);
retry:
  for (q=0; q < NR_SCHED_QUEUES; q++) {	
	if(!(rp = rdy_head[q])) {
		TRACE(VF_PICKPROC, printf("cpu %d queue %d empty\n", cpuid, q););
		continue;
	}
	if(!proc_is_runnable(rp)) {
		/* rp may not be runnable if we received a dequeue IPI during
		 * the pick_proc. In this case simply retry the pick proc. */
		goto retry;
	}
	if (priv(rp)->s_flags & BILLABLE)	 	
		get_cpulocal_var(bill_ptr) = rp; /* bill for system time */
	return rp;
  }
  return NULL;
}

/*===========================================================================*
 *				endpoint_lookup				     *
 *===========================================================================*/
struct proc *endpoint_lookup(endpoint_t e)
{
	int n;

	if(!isokendpt(e, &n)) return NULL;

	return proc_addr(n);
}

/*===========================================================================*
 *				isokendpt_f				     *
 *===========================================================================*/
#if DEBUG_ENABLE_IPC_WARNINGS
int isokendpt_f(const char * file, int line, endpoint_t e, int * p,
	const int fatalflag)
#else
int isokendpt_f(endpoint_t e, int * p, const int fatalflag)
#endif
{
	int ok = 0;
	/* Convert an endpoint number into a process number.
	 * Return nonzero if the process is alive with the corresponding
	 * generation number, zero otherwise.
	 *
	 * This function is called with file and line number by the
	 * isokendpt_d macro if DEBUG_ENABLE_IPC_WARNINGS is defined,
	 * otherwise without. This allows us to print the where the
	 * conversion was attempted, making the errors verbose without
	 * adding code for that at every call.
	 * 
	 * If fatalflag is nonzero, we must panic if the conversion doesn't
	 * succeed.
	 */
	*p = _ENDPOINT_P(e);
	ok = 0;
	if(isokprocn(*p) && !isemptyn(*p) && proc_addr(*p)->p_endpoint == e)
		ok = 1;
	if(!ok && fatalflag)
		panic("invalid endpoint: %d",  e);
	return ok;
}

static void notify_scheduler(struct proc *p)
{
	message m_no_quantum;
	int err;

	assert(proc_locked(p));
	assert(proc_locked(p->p_scheduler));

	assert(!proc_kernel_scheduler(p));

	/* dequeue the process */
	RTS_SET(p, RTS_NO_QUANTUM);
	/*
	 * Notify the process's scheduler that it has run out of
	 * quantum. This is done by sending a message to the scheduler
	 * on the process's behalf
	 */
	m_no_quantum.m_source = p->p_endpoint;
	m_no_quantum.m_type   = SCHEDULING_NO_QUANTUM;
	m_no_quantum.m_krn_lsys_schedule.acnt_queue = cpu_time_2_ms(p->p_accounting.time_in_queue);
	m_no_quantum.m_krn_lsys_schedule.acnt_deqs      = p->p_accounting.dequeues;
	m_no_quantum.m_krn_lsys_schedule.acnt_ipc_sync  = p->p_accounting.ipc_sync;
	m_no_quantum.m_krn_lsys_schedule.acnt_ipc_async = p->p_accounting.ipc_async;
	m_no_quantum.m_krn_lsys_schedule.acnt_preempt   = p->p_accounting.preempted;
	m_no_quantum.m_krn_lsys_schedule.acnt_cpu       = cpuid;
	m_no_quantum.m_krn_lsys_schedule.acnt_cpu_load  = cpu_load();

	/* Reset accounting */
	reset_proc_accounting(p);

	/* We have BKL. */
	if ((err = mini_send_no_lock(p, p->p_scheduler->p_endpoint,
					&m_no_quantum, FROM_KERNEL))) {
		panic("WARNING: Scheduling: mini_send returned %d\n", err);
	}
}

void proc_no_time(struct proc * p)
{
	assert(proc_locked(p));
	if (!proc_kernel_scheduler(p) && priv(p)->s_flags & PREEMPTIBLE) {
		/* this dequeues the process */
		unlock_proc(p);
		lock_two_procs(p,p->p_scheduler);
		/* Re-check the condition, it might have changed in the meantime
		 */
		if(!p->p_cpu_time_left)
			notify_scheduler(p);
		/* Keep the lock on p for switch_to_user. */
		unlock_proc(p->p_scheduler);
	}
	else {
		/*
		 * non-preemptible processes only need their quantum to
		 * be renewed. In fact, they by pass scheduling
		 */
		p->p_cpu_time_left = ms_2_cpu_time(p->p_quantum_size_ms);
#if DEBUG_RACE
		RTS_SET(p, RTS_PREEMPTED);
		RTS_UNSET(p, RTS_PREEMPTED);
#endif
	}
}

void reset_proc_accounting(struct proc *p)
{
  p->p_accounting.preempted = 0;
  p->p_accounting.ipc_sync  = 0;
  p->p_accounting.ipc_async = 0;
  p->p_accounting.dequeues  = 0;
  p->p_accounting.time_in_queue = 0;
  p->p_accounting.enter_queue = 0;
}
	
void copr_not_available_handler(void)
{
	struct proc * p;
	struct proc ** local_fpu_owner;
	/*
	 * Disable the FPU exception (both for the kernel and for the process
	 * once it's scheduled), and initialize or restore the FPU state.
	 */

	disable_fpu_exception();

	p = get_cpulocal_var(proc_ptr);
	lock_proc(p);

	/* if FPU is not owned by anyone, do not store anything */
	local_fpu_owner = get_cpulocal_var_ptr(fpu_owner);
	if (*local_fpu_owner != NULL) {
		assert(*local_fpu_owner != p);
		save_local_fpu(*local_fpu_owner, FALSE /*retain*/);
	}

	/*
	 * restore the current process' state and let it run again, do not
	 * schedule!
	 */
	if (restore_fpu(p) != OK) {
		/* Restoring FPU state failed. This is always the process's own
		 * fault. Send a signal, and schedule another process instead.
		 */
		*local_fpu_owner = NULL;		/* release FPU */
		cause_sig_deferred(proc_nr(p), SIGFPE);
		unlock_proc(p);
		return;
	}

	*local_fpu_owner = p;
	unlock_proc(p);
	context_stop(proc_addr(KERNEL));
	restore_user_context(p);
	NOT_REACHABLE;
}

void release_fpu(struct proc * p) {
	struct proc ** fpu_owner_ptr;

	fpu_owner_ptr = get_cpu_var_ptr(p->p_cpu, fpu_owner);

	if (*fpu_owner_ptr == p)
		*fpu_owner_ptr = NULL;
}

void ser_dump_proc(void)
{
        struct proc *pp;

        for (pp= BEG_PROC_ADDR; pp < END_PROC_ADDR; pp++)
        {
                if (isemptyp(pp))
                        continue;
                print_proc_recursive(pp);
        }
}

void sink(void)
{
	/* Do nothing. */
}

void _rts_set(struct proc *p,int flag,int lockflag)
{
	if(lockflag==1)
		assert(proc_locked_borrow(p));
	else if(lockflag==2)
		assert(proc_locked(p));
	p->p_rts_flags |= (flag);
	p->__gdb_last_cpu_flag = cpuid;
	p->__gdb_line = __LINE__;
	p->__gdb_file = __FILE__;
	if(!proc_is_runnable(p)&&p->p_enqueued) {
		if(cpuid!=p->p_cpu)
			smp_dequeue_task(p);
		else
			dequeue(p);
	}
	assert(!p->p_enqueued);
}

void _rts_unset(struct proc *p,int flag,int lockflag)
{
	int rts;
	if(lockflag==1)
		assert(proc_locked_borrow(p));
	else if(lockflag==2)
		assert(proc_locked(p));
	rts = p->p_rts_flags;
	p->p_rts_flags &= ~(flag);
	p->__gdb_last_cpu_flag = cpuid;
	p->__gdb_line = __LINE__;
	p->__gdb_file = __FILE__;
	if(!rts_f_is_runnable(rts) && proc_is_runnable(p)) {
		enqueue(p);
	}
}

void _rts_setflags(struct proc *p,int flag)
{
	assert(proc_locked(p));
	p->p_rts_flags = (flag);
	if(proc_is_runnable(p) && (flag)) {
		if(cpuid!=p->p_cpu)
			smp_dequeue_task(p);
		else
			dequeue(p);
	}
}

void lock_proc(struct proc *p)
{
	/* Passing NULL may happens when "prefetching" in mini_receive. */
	if(!p)
		return;
	/* For now we bypass the reentrant locks. */
	spinlock_lock(&(p->p_lock.lock));
	p->p_lock.owner = cpuid;
}

void unlock_proc(struct proc *p)
{
	/* Passing NULL may happens when "prefetching" in mini_receive. */
	if(!p)
		return;
	/* For now we bypass the reentrant locks. */
	assert(p->p_lock.owner==cpuid);
	p->p_lock.owner = -1;
	spinlock_unlock(&(p->p_lock.lock));
}

int proc_locked(const struct proc *p)
{
	/* Assert if a proc is locked by the current cpu.
	 * We don't need to lock pseudo processes. */
	if(!p)
		return 1;
	else if(p->p_endpoint==KERNEL||p->p_endpoint==SYSTEM)
		return 1;
	else
		return (p->p_lock.lock.val==1&&p->p_lock.owner==cpuid);
}

int proc_locked_borrow(const struct proc *p)
{
	/* Assert if a proc is locked by a remote cpu.
	 * We don't need to lock pseudo processes. */
	if(!p)
		return 1;
	else if(p->p_endpoint==KERNEL||p->p_endpoint==SYSTEM)
		return 1;
	else
		return (p->p_lock.lock.val==1&&p->p_lock.owner!=cpuid);
}

void lock_two_procs(struct proc *p1,struct proc *p2)
{
	if(p1<p2) {
		lock_proc(p1);
		lock_proc(p2);
	} else if(p2<p1) {
		lock_proc(p2);
		lock_proc(p1);
	} else {
		/* p1==p2. */
		lock_proc(p1);
	}
}

void unlock_two_procs(struct proc *p1,struct proc *p2)
{
	if(p1<p2) {
		unlock_proc(p2);
		unlock_proc(p1);
	} else if(p2<p1) {
		unlock_proc(p1);
		unlock_proc(p2);
	} else {
		/* p1==p2. */
		unlock_proc(p1);
	}
}

static void _sort4(struct proc *sorted[4],struct proc *p1,struct proc *p2,struct proc *p3,struct proc *p4)
{
	struct proc *left[2],*right[2];	/* Subsets. */
	int i,left_i,right_i;
	struct proc *left_head,*right_head;

	if(p1<p2) {
		left[0] = p1;
		left[1] = p2;
	} else {
		left[0] = p2;
		left[1] = p1;
	}

	if(p3<p4) {
		right[0] = p3;
		right[1] = p4;
	} else {
		right[0] = p4;
		right[1] = p3;
	}

	left_i = 0;
	right_i = 0;
	for(i=0;i<4;++i) {
		if(left_i<2)
			left_head = left[left_i];
		if(right_i<2)
			right_head = right[right_i];
		if(left_i<2&&right_i<2) {
			/* Both heads are valid. */
			if(left_head<right_head) {
				sorted[i] = left_head;
				left_i++;
			} else {
				sorted[i] = right_head;
				right_i++;
			}
		} else if(left_i<2) {
			sorted[i] = left_head;
			left_i++;
		} else if(right_i<2) {
			sorted[i] = right_head;
			right_i++;
		} else {
			panic("Sorting failed.");
		}
	}

}

void lock_four_procs(struct proc *p1,struct proc *p2,struct proc *p3,struct proc *p4)
{
	struct proc *sorted[4];
	int i;
	struct proc *last;

	_sort4(sorted,p1,p2,p3,p4);

	last = NULL;
	for(i=0;i<4;++i) {
		assert(last<=sorted[i]);
		if(sorted[i]&&(i==0||sorted[i-1]!=sorted[i]))
			lock_proc(sorted[i]);
		last = sorted[i];
	}
}

void unlock_four_procs(struct proc *p1,struct proc *p2,struct proc *p3,struct proc *p4)
{
	struct proc *sorted[4];
	struct proc *last;
	int i;
	_sort4(sorted,p1,p2,p3,p4);
	last = NULL;
	for(i=0;i<4;++i) {
		assert(last<=sorted[i]);
		if(sorted[i]&&(i==0||sorted[i-1]!=sorted[i]))
			unlock_proc(sorted[i]);
		last = sorted[i];
	}
}

void lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	lock_four_procs(p1,p2,p3,NULL);
}

void unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	unlock_four_procs(p1,p2,p3,NULL);
}

struct proc *proc_for_endpoint(endpoint_t endpt)
{
	int proc_nr;
	if(!isokendpt(endpt, &proc_nr)) {
		panic("Invalid enpoint in proc_for_endpoint");
	} else {
		return proc_addr(proc_nr);
	}
}

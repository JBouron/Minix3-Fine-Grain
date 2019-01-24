/* The kernel call implemented in this file:
 *   m_type:	SYS_VMCTL
 *
 * The parameters for this kernel call are:
 *   	SVMCTL_WHO	which process
 *    	SVMCTL_PARAM	set this setting (VMCTL_*)
 *    	SVMCTL_VALUE	to this value
 */

#include "kernel/system.h"
#include "kernel/vm.h"
#include <assert.h>

/*===========================================================================*
 *				do_vmctl				     *
 *===========================================================================*/
int do_vmctl_impl(struct proc * caller, message * m_ptr)
{
  int proc_nr;
  endpoint_t ep = m_ptr->SVMCTL_WHO;
  struct proc *p, *rp, **rpp, *target, *head;

  if(ep == SELF) { ep = caller->p_endpoint; }

  if(!isokendpt(ep, &proc_nr)) {
	printf("do_vmctl: unexpected endpoint %d from VM\n", ep);
	return EINVAL;
  }

  p = proc_addr(proc_nr);

  switch(m_ptr->SVMCTL_PARAM) {
	case VMCTL_CLEAR_PAGEFAULT:
		lock_proc(p);
		assert(RTS_ISSET(p,RTS_PAGEFAULT));
		RTS_UNSET(p, RTS_PAGEFAULT);
		unlock_proc(p);
		return OK;
	case VMCTL_MEMREQ_GET:
		/* Send VM the information about the memory request. We can
		 * not simply send the first request on the list, because IPC
		 * filters may forbid VM from getting requests for particular
		 * sources. However, IPC filters are used only in rare cases.
		 *
		 * UPDATE Justinien Bouron: The IPC filters for VM are disabled
		 * because they were not used anyway. Thus we can simply take
		 * the first proc in the list.
		 */

retry_vmrequest_head:
		/* Careful with the lock ordering, first the proc *then* the
		 * vmrequest_lock. */
		head = vmrequest;
		lock_proc(head);
		lock_vmrequest();
		/* Check that no proc enqueued itself in the meantime. */
		if(head!=vmrequest) {
			/* Somebody put itself at the head of the vmrequest
			 * queue, try again. */
			unlock_vmrequest();
			unlock_proc(head);
			goto retry_vmrequest_head;
		}

		if(head==NULL) {
			/* No need to unlock the null proc. */
			unlock_vmrequest();
			return ENOENT;
		}

		assert(RTS_ISSET(head, RTS_VMREQUEST));

		okendpt(head->p_vmrequest.target, &proc_nr);
		target = proc_addr(proc_nr);

		/* Check against IPC filters. */
		if (!allow_ipc_filtered_memreq(head, target))
			panic("NOT IMPLEMENTED");

		/* Reply with request fields. */
		if (head->p_vmrequest.req_type != VMPTYPE_CHECK)
			panic("VMREQUEST wrong type");

		m_ptr->SVMCTL_MRG_TARGET= head->p_vmrequest.target;
		m_ptr->SVMCTL_MRG_ADDR= head->p_vmrequest.params.check.start;
		m_ptr->SVMCTL_MRG_LENGTH= head->p_vmrequest.params.check.length;
		m_ptr->SVMCTL_MRG_FLAG= head->p_vmrequest.params.check.writeflag;
		m_ptr->SVMCTL_MRG_REQUESTOR= (void *) head->p_endpoint;

		head->p_vmrequest.vmresult = VMSUSPEND;

		/* Remove from request chain. */
		vmrequest = head->p_vmrequest.nextrequestor;

		const int req_type = head->p_vmrequest.req_type;

		/* Unlock the vmrequest and the head. */
		unlock_vmrequest();
		unlock_proc(head);

		return req_type;

	case VMCTL_MEMREQ_REPLY:
  		okendpt(p->p_vmrequest.target, &proc_nr);
		target = proc_addr(proc_nr);

		lock_two_procs(p,target);

		assert(RTS_ISSET(p, RTS_VMREQUEST));
		assert(p->p_vmrequest.vmresult == VMSUSPEND);
		p->p_vmrequest.vmresult = m_ptr->SVMCTL_VALUE;
		assert(p->p_vmrequest.vmresult != VMSUSPEND);

		switch(p->p_vmrequest.type) {
		case VMSTYPE_KERNELCALL:
			/*
			 * we will have to resume execution of the kernel call
			 * as soon the scheduler picks up this process again
			 */
			p->p_misc_flags |= MF_KCALL_RESUME;
			break;
		case VMSTYPE_DELIVERMSG:
			assert(p->p_misc_flags & MF_DELIVERMSG);
			assert(p == target);
			assert(RTS_ISSET(p, RTS_VMREQUEST));
			break;
		case VMSTYPE_MAP:
			assert(RTS_ISSET(p, RTS_VMREQUEST));
			break;
		default:
			panic("strange request type: %d",p->p_vmrequest.type);
		}

		RTS_UNSET(p, RTS_VMREQUEST);
		unlock_two_procs(p,target);
		return OK;

	case VMCTL_KERN_PHYSMAP:
	{
		int i = m_ptr->SVMCTL_VALUE;
		return arch_phys_map(i,
			(phys_bytes *) &m_ptr->SVMCTL_MAP_PHYS_ADDR,
			(phys_bytes *) &m_ptr->SVMCTL_MAP_PHYS_LEN,
			&m_ptr->SVMCTL_MAP_FLAGS);
	}
	case VMCTL_KERN_MAP_REPLY:
	{
		return arch_phys_map_reply(m_ptr->SVMCTL_VALUE,
			(vir_bytes) m_ptr->SVMCTL_MAP_VIR_ADDR);
	}
	case VMCTL_VMINHIBIT_SET:
		/* check if we must stop a process on a different CPU */
		lock_proc(p);
#if CONFIG_SMP
		if (p->p_cpu != cpuid) {
			smp_schedule_vminhibit(p);
		} else
#endif
			RTS_SET(p, RTS_VMINHIBIT);
#if CONFIG_SMP
		p->p_misc_flags |= MF_FLUSH_TLB;
#endif
		unlock_proc(p);
		return OK;
	case VMCTL_VMINHIBIT_CLEAR:
		lock_proc(p);
		assert(RTS_ISSET(p, RTS_VMINHIBIT));
		/*
		 * the processes is certainly not runnable, no need to tell its
		 * cpu
		 */
		RTS_UNSET(p, RTS_VMINHIBIT);
#ifdef CONFIG_SMP
		struct priv *privp;
		p->p_misc_flags &= ~MF_SENDA_VM_MISS;
		privp = priv(p);
		if(privp)
			try_deliver_senda(p,(asynmsg_t*)privp->s_asyntab,privp->s_asynsize,1);
		/*
		 * We don't know whether kernel has the changed mapping
		 * installed to access userspace memory. And if so, on what CPU.
		 * More over we don't know what mapping has changed and how and
		 * therefore we must invalidate all mappings we have anywhere.
		 * Next time we map memory, we map it fresh.
		 */
		bits_fill(p->p_stale_tlb, CONFIG_MAX_CPUS);
#endif
		unlock_proc(p);
		return OK;
	case VMCTL_CLEARMAPCACHE:
		/* VM says: forget about old mappings we have cached. */
		mem_clear_mapcache();
		return OK;
	case VMCTL_BOOTINHIBIT_CLEAR:
		lock_proc(p);
		RTS_UNSET(p, RTS_BOOTINHIBIT);
		unlock_proc(p);
		return OK;
  }

  /* Try architecture-specific vmctls. */
  return arch_do_vmctl(m_ptr, p);
}

int do_vmctl(struct proc * caller, message * m_ptr)
{
	const int res = do_vmctl_impl(caller,m_ptr);
	/* kernel_call_finish expects the lock on caller. */
	lock_proc(caller);
	return res;
}

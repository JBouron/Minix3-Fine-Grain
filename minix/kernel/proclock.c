/* This file is to be included by proc.c.
 * Ideally this should be in a standalone .c file with an header file
 * containing the declaration of the proclock_impl_t struct, but the compiler
 * was not really happy with that... TODO
 */

struct proclock_impl_t proclock_impl;

/* ========================================================================= */
/*		Entry points  						     */
/* ========================================================================= */

/*
 * Set the owner of `p` to this cpu. Assert that `p` is not currently owned by
 * another cpu.
 */
static void _set_owner(struct proc *p)
{
#ifdef PROC_LOCK_CHECKS
	assert(p->p_owner==-1);
	p->p_owner = cpuid;
#endif
}

/*
 * Give up the ownershipt of `p`. Assert that `p` in indeed owned by the
 * current cpu.
 */
static void _reset_owner(struct proc *p)
{
#ifdef PROC_LOCK_CHECKS
	assert(p->p_owner==cpuid);
	p->p_owner = -1;
#endif
}

void lock_proc(struct proc *p)
{
	if(p) {
		proclock_impl.lock_proc(p);
		_set_owner(p);
	}
}

void unlock_proc(struct proc *p)
{
	if(p) {
		_reset_owner(p);
		proclock_impl.unlock_proc(p);
	}
}

void lock_two_procs(struct proc *p1,struct proc *p2)
{
	/* We need to handle the NULL pointers as well as the case where
	 * p1==p2. */
	if(!p1) {
		lock_proc(p2);
	} else if(!p2) {
		lock_proc(p1);
	} else if(p1<p2) {
		proclock_impl.lock_two_procs(p1,p2);
		_set_owner(p1);
		_set_owner(p2);
	} else if(p2<p1) {
		proclock_impl.lock_two_procs(p2,p1);
		_set_owner(p1);
		_set_owner(p2);
	} else {
		/* p1==p2. */
		lock_proc(p1);
	}
}

void unlock_two_procs(struct proc *p1,struct proc *p2)
{
	if(!p1) {
		unlock_proc(p2);
	} else if(!p2) {
		unlock_proc(p1);
	} else if(p1<p2) {
		_reset_owner(p1);
		_reset_owner(p2);
		proclock_impl.unlock_two_procs(p1,p2);
	} else if(p2<p1) {
		_reset_owner(p1);
		_reset_owner(p2);
		proclock_impl.unlock_two_procs(p2,p1);
	} else {
		/* p1==p2. */
		unlock_proc(p1);
	}
}

static void _sort3(struct proc **dest,struct proc *p1,struct proc *p2,struct proc *p3)
{
	struct proc *const min12 = p1<p2?p1:p2;
	struct proc *const min23 = p2<p3?p2:p3;
	struct proc *const min13 = p1<p3?p1:p3;
	struct proc *const max12 = p1>p2?p1:p2;
	struct proc *const max23 = p2>p3?p2:p3;
	struct proc *const max13 = p1>p3?p1:p3;

	if(min12==min23) {
		dest[0] = p2;
		dest[1] = min13;
		dest[2] = max13;
	} else if(min23==min13) {
		dest[0] = p3;
		dest[1] = min12;
		dest[2] = max12;
	} else {
		dest[0] = p1;
		dest[1] = min23;
		dest[2] = max23;
	}
}

void lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	if(!p1) {
		lock_two_procs(p2,p3);
	} else if(p1==p2||p2==p3||!p2) {
		lock_two_procs(p1,p3);
	} else if(p1==p3||!p3) {
		lock_two_procs(p1,p2);
	} else {
		struct proc *sorted[3];
		_sort3(sorted,p1,p2,p3);
		proclock_impl.lock_three_procs(sorted[0],sorted[1],sorted[2]);
		_set_owner(p1);
		_set_owner(p2);
		_set_owner(p3);
	}
}

void unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	if(!p1) {
		unlock_two_procs(p2,p3);
	} else if(p1==p2||p2==p3||!p2) {
		unlock_two_procs(p1,p3);
	} else if(p1==p3||!p3) {
		unlock_two_procs(p1,p2);
	} else {
		struct proc *sorted[3];
		_sort3(sorted,p1,p2,p3);
		_reset_owner(p1);
		_reset_owner(p2);
		_reset_owner(p3);
		proclock_impl.unlock_three_procs(sorted[0],sorted[1],sorted[2]);
	}
}

void assert_proc_locked(const struct proc *p)
{
	/* Assert if a proc is locked by the current cpu.
	 * We don't need to lock pseudo processes. */
#ifdef PROC_LOCK_CHECKS
	if(p&&p->p_endpoint!=KERNEL&&p->p_endpoint!=SYSTEM)
		assert(p->p_owner==cpuid);
#endif
}

void assert_proc_locked_borrow(const struct proc *p)
{
	/* Assert if a proc is locked by a remote cpu.
	 * We don't need to lock pseudo processes. */
#ifdef PROC_LOCK_CHECKS
	if(p&&p->p_endpoint!=KERNEL&&p->p_endpoint!=SYSTEM) {
		const int owner = p->p_owner;
		assert(owner!=-1&&owner!=cpuid);
	}
#endif
}

/* ========================================================================= */
/*		SPINLOCK implementation					     */
/* ========================================================================= */
void _sl_lock_proc(struct proc *p)
{
	spinlock_lock(&(p->p_spinlock));
}

void _sl_unlock_proc(struct proc *p)
{
	spinlock_unlock(&(p->p_spinlock));
}

void _sl_lock_two_procs(struct proc *p1,struct proc *p2)
{
	_sl_lock_proc(p1);
	_sl_lock_proc(p2);
}

void _sl_unlock_two_procs(struct proc *p1,struct proc *p2)
{
	_sl_unlock_proc(p1);
	_sl_unlock_proc(p2);
}

void _sl_lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_sl_lock_proc(p1);
	_sl_lock_proc(p2);
	_sl_lock_proc(p3);
}

void _sl_unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_sl_unlock_proc(p1);
	_sl_unlock_proc(p2);
	_sl_unlock_proc(p3);
}

/* ========================================================================= */
/*		TICKETLOCK implementation			             */
/* ========================================================================= */
void _tl_lock_proc(struct proc *p)
{
	ticketlock_lock(&(p->p_ticketlock));
}

void _tl_unlock_proc(struct proc *p)
{
	ticketlock_unlock(&(p->p_ticketlock));
}

void _tl_lock_two_procs(struct proc *p1,struct proc *p2)
{
	_tl_lock_proc(p1);
	_tl_lock_proc(p2);
}

void _tl_unlock_two_procs(struct proc *p1,struct proc *p2)
{
	_tl_unlock_proc(p1);
	_tl_unlock_proc(p2);
}

void _tl_lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_tl_lock_proc(p1);
	_tl_lock_proc(p2);
	_tl_lock_proc(p3);
}

void _tl_unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_tl_unlock_proc(p1);
	_tl_unlock_proc(p2);
	_tl_unlock_proc(p3);
}

/* ========================================================================= */
/*		MCSLOCK implementation			                     */
/* ========================================================================= */
static mcs_node_t *_get_mcs_node(struct proc *p)
{
	int idx;
	if(p==&get_cpulocal_var(idle_proc)) {
		idx = 0;
	} else {
		const ptrdiff_t addr_off = p-(&proc[0]);
		/* +1 for the idle proc. */
		idx = addr_off+1;
	}
	return &(get_cpulocal_var(mcs_nodes)[idx]);
}

void _mcs_lock_proc(struct proc *p)
{
	mcslock_t *const proc_lock = &(p->p_mcslock);
	mcs_node_t *const I = _get_mcs_node(p);
	mcslock_lock(proc_lock,I);
}

void _mcs_unlock_proc(struct proc *p)
{
	mcslock_t *const proc_lock = &(p->p_mcslock);
	mcs_node_t *const I = _get_mcs_node(p);
	mcslock_unlock(proc_lock,I);
}

void _mcs_lock_two_procs(struct proc *p1,struct proc *p2)
{
	_mcs_lock_proc(p1);
	_mcs_lock_proc(p2);
}

void _mcs_unlock_two_procs(struct proc *p1,struct proc *p2)
{
	_mcs_unlock_proc(p1);
	_mcs_unlock_proc(p2);
}

void _mcs_lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_mcs_lock_proc(p1);
	_mcs_lock_proc(p2);
	_mcs_lock_proc(p3);
}

void _mcs_unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	_mcs_unlock_proc(p1);
	_mcs_unlock_proc(p2);
	_mcs_unlock_proc(p3);
}

/* ========================================================================= */
/*		NO-LOCK implementation					     */
/* ========================================================================= */

void _nl_lock_proc(struct proc *p)
{
}

void _nl_unlock_proc(struct proc *p)
{
}

void _nl_lock_two_procs(struct proc *p1,struct proc *p2)
{
}

void _nl_unlock_two_procs(struct proc *p1,struct proc *p2)
{
}

void _nl_lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
}

void _nl_unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
}

/* ========================================================================= */
/*		INIT implementation					     */
/* ========================================================================= */
void init_proclock_impl(const char *const name)
{
	printf("Using %s for proc locks.\n",name);
	if(!strcmp(name,"spinlock")) {
		proclock_impl = (struct proclock_impl_t) {
			.lock_proc          = _sl_lock_proc,
			.unlock_proc        = _sl_unlock_proc,
			.lock_two_procs     = _sl_lock_two_procs,
			.unlock_two_procs   = _sl_unlock_two_procs,
			.lock_three_procs   = _sl_lock_three_procs,
			.unlock_three_procs = _sl_unlock_three_procs,
		};
	} else if(!strcmp(name,"ticketlock")){
		proclock_impl = (struct proclock_impl_t) {
			.lock_proc          = _tl_lock_proc,
			.unlock_proc        = _tl_unlock_proc,
			.lock_two_procs     = _tl_lock_two_procs,
			.unlock_two_procs   = _tl_unlock_two_procs,
			.lock_three_procs   = _tl_lock_three_procs,
			.unlock_three_procs = _tl_unlock_three_procs,
		};
	} else if(!strcmp(name,"mcs")){
		proclock_impl = (struct proclock_impl_t) {
			.lock_proc          = _mcs_lock_proc,
			.unlock_proc        = _mcs_unlock_proc,
			.lock_two_procs     = _mcs_lock_two_procs,
			.unlock_two_procs   = _mcs_unlock_two_procs,
			.lock_three_procs   = _mcs_lock_three_procs,
			.unlock_three_procs = _mcs_unlock_three_procs,
		};
	} else if(!strcmp(name,"nolock")){
		proclock_impl = (struct proclock_impl_t) {
			.lock_proc          = _nl_lock_proc,
			.unlock_proc        = _nl_unlock_proc,
			.lock_two_procs     = _nl_lock_two_procs,
			.unlock_two_procs   = _nl_unlock_two_procs,
			.lock_three_procs   = _nl_lock_three_procs,
			.unlock_three_procs = _nl_unlock_three_procs,
		};
	} else {
		panic("Unknonwn proc lock implementation name.");
	}
}

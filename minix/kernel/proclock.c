/* This file is to be included by proc.c.
 * Ideally this should be in a standalone .c file with an header file
 * containing the declaration of the proclock_impl_t struct, but the compiler
 * was not really happy with that... TODO
 */

struct proclock_impl_t proclock_impl;
/* ========================================================================= */
/*		Entry points  						     */
/* ========================================================================= */
void lock_proc(struct proc *p)
{
	if(p)
		proclock_impl.lock_proc(p);
}

void unlock_proc(struct proc *p)
{
	if(p)
		proclock_impl.unlock_proc(p);
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
	} else if(p2<p1) {
		proclock_impl.lock_two_procs(p2,p1);
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
		proclock_impl.unlock_two_procs(p1,p2);
	} else if(p2<p1) {
		proclock_impl.unlock_two_procs(p2,p1);
	} else {
		/* p1==p2. */
		unlock_proc(p1);
	}
}

static void _sort3(struct proc **dest,struct proc *p1,struct proc *p2,struct proc *p3)
{
	assert(p1!=p2&&p2!=p3&&p3!=p1);
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
		assert(min12==min13);
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
		proclock_impl.unlock_three_procs(sorted[0],sorted[1],sorted[2]);
	}
}

int proc_locked(const struct proc *p)
{
#ifdef CHECK_PROC_LOCKS
	/* Assert if a proc is locked by the current cpu.
	 * We don't need to lock pseudo processes. */
	if(!p)
		return 1;
	else if(p->p_endpoint==KERNEL||p->p_endpoint==SYSTEM)
		return 1;
	else
		return proclock_impl.proc_locked(p);
#else
	return 1;
#endif
}

int proc_locked_borrow(const struct proc *p)
{
#ifdef CHECK_PROC_LOCKS
	/* Assert if a proc is locked by a remote cpu.
	 * We don't need to lock pseudo processes. */
	if(!p)
		return 1;
	else if(p->p_endpoint==KERNEL||p->p_endpoint==SYSTEM)
		return 1;
	else
		return proclock_impl.proc_locked_borrow(p);
#else
	return 1;
#endif
}

/* ========================================================================= */
/*		SPINLOCK implementation					     */
/* ========================================================================= */
void _sl_lock_proc(struct proc *p)
{
	assert(p);
	int tries = 0;
retry:
	tries++;
	while(p->p_lock.lock.val) {}

	/* Try to lock p1. */
	if(!arch_spinlock_test(&(p->p_lock.lock.val))) {
		goto retry;
	}
	p->p_n_lock++;
	p->p_n_tries += tries;
	p->p_lock.owner = cpuid;
}

void _sl_unlock_proc(struct proc *p)
{
	assert(p);
	assert(p->p_lock.owner==cpuid);
	p->p_lock.owner = -1;
	/* For now we bypass the reentrant locks. */
	spinlock_unlock(&(p->p_lock.lock));
}

void _sl_lock_two_procs(struct proc *p1,struct proc *p2)
{
	assert(p1&&p2&&p1<p2);

	/* Perform two-way test-test&set. */
retry:
	while(p1->p_lock.lock.val&&p2->p_lock.lock.val) {}

	/* Try to lock p1. */
	if(!arch_spinlock_test(&(p1->p_lock.lock.val))) {
		goto retry;
	} else {
		/* We have the lock, update owner. */
		p1->p_lock.owner = cpuid;
	}

	/* Try to lock p2. */
	if(!arch_spinlock_test(&(p2->p_lock.lock.val))) {
		/* Cannot lock p2, don't hold p1 and return to the test loop. */
		proclock_impl.unlock_proc(p1);
		goto retry;
	} else {
		/* We have the lock, update owner. */
		p2->p_lock.owner = cpuid;
	}
}

void _sl_unlock_two_procs(struct proc *p1,struct proc *p2)
{
	assert(p1&&p2&&p1<p2);
	proclock_impl.unlock_proc(p1);
	proclock_impl.unlock_proc(p2);
}

void _sl_lock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	assert(p1&&p2&&p3&&p1<p2&&p2<p3);

	/* Perform two-way test-test&set. */
retry:
	while(p1->p_lock.lock.val&&
	      p2->p_lock.lock.val&&
	      p3->p_lock.lock.val) {}

	/* Try to lock p1. */
	if(!arch_spinlock_test(&(p1->p_lock.lock.val))) {
		goto retry;
	} else {
		/* We have the lock, update owner. */
		p1->p_lock.owner = cpuid;
	}

	/* Try to lock p2. */
	if(!arch_spinlock_test(&(p2->p_lock.lock.val))) {
		/* Cannot lock p2, don't hold p1 and return to the test loop. */
		proclock_impl.unlock_proc(p1);
		goto retry;
	} else {
		/* We have the lock, update owner. */
		p2->p_lock.owner = cpuid;
	}

	/* Try to lock p3. */
	if(!arch_spinlock_test(&(p3->p_lock.lock.val))) {
		/* Cannot lock p2, don't hold p1 and return to the test loop. */
		proclock_impl.unlock_two_procs(p1,p2);
		goto retry;
	} else {
		/* We have the lock, update owner. */
		p3->p_lock.owner = cpuid;
	}
}

void _sl_unlock_three_procs(struct proc *p1,struct proc *p2,struct proc *p3)
{
	assert(p1&&p2&&p3&&p1<p2&&p2<p3);
	proclock_impl.unlock_proc(p1);
	proclock_impl.unlock_proc(p2);
	proclock_impl.unlock_proc(p3);
}

int _sl_proc_locked(const struct proc *p)
{
	return (p->p_lock.lock.val==1&&p->p_lock.owner==cpuid);
}

int _sl_proc_locked_borrow(const struct proc *p)
{
	return (p->p_lock.lock.val==1&&p->p_lock.owner!=cpuid);
}

/* ========================================================================= */
/*		INIT implementation					     */
/* ========================================================================= */
void init_proclock_impl(const char *const name)
{
	if(!strcmp(name,"spinlock")) {
		proclock_impl = (struct proclock_impl_t) {
			.lock_proc          = _sl_lock_proc,
			.unlock_proc        = _sl_unlock_proc,
			.lock_two_procs     = _sl_lock_two_procs,
			.unlock_two_procs   = _sl_unlock_two_procs,
			.lock_three_procs   = _sl_lock_three_procs,
			.unlock_three_procs = _sl_unlock_three_procs,
			.proc_locked        = _sl_proc_locked,
			.proc_locked_borrow = _sl_proc_locked_borrow,
		};
	} else {
		panic("Unknonwn proc lock implementation name.");
	}
}

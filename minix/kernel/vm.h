
#ifndef _VM_H
#define _VM_H 1

/* Pseudo error codes */
#define VMSUSPEND       (-996)
#define EFAULT_SRC	(-995)
#define EFAULT_DST	(-994)

#define PHYS_COPY_CATCH(src, dst, size, a) {	\
	get_cpulocal_var(catch_pagefaults)++;			\
	a = phys_copy(src, dst, size);		\
	get_cpulocal_var(catch_pagefaults)--;			\
	}

#endif



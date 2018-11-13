#ifndef GLO_H
#define GLO_H

/* Global variables used in the kernel. This file contains the declarations;
 * storage space for the variables is allocated in table.c, because EXTERN is
 * defined as extern unless the _TABLE definition is seen. We rely on the 
 * compiler's default initialization (0) for several global variables. 
 */
#ifdef _TABLE
#undef EXTERN
#define EXTERN
#endif

#include <minix/config.h>
#include <minix/ipcconst.h>
#include <machine/archtypes.h>
#include "archconst.h"
#include "config.h"
#include "debug.h"
#include "ktrace.h"
#include "arch_smp.h"
#include "kernel/spinlock.h"

/* Kernel information structures. This groups vital kernel information. */
extern struct kinfo kinfo;		  /* kernel information for services */
extern struct machine machine;		  /* machine info for services */
extern struct kmessages kmessages;  	  /* diagnostic messages in kernel */
extern struct loadinfo loadinfo;	  /* status of load average */
extern struct kuserinfo kuserinfo;	  /* kernel information for users */
extern struct arm_frclock arm_frclock;	  /* ARM free-running timer info */
extern struct kclockinfo kclockinfo;	  /* clock information */
extern struct minix_kerninfo minix_kerninfo;

EXTERN struct k_randomness krandom; 	/* gather kernel random information */

EXTERN vir_bytes minix_kerninfo_user;

#define kmess kmessages
#define kloadinfo loadinfo

#define system_hz (kclockinfo.hz)		/* HZ value (alias) */

/* Process scheduling information and the kernel reentry count. */
EXTERN struct proc *vmrequest;  /* first process on vmrequest queue */
EXTERN unsigned lost_ticks;	/* clock ticks counted outside clock task */
EXTERN char *ipc_call_names[IPCNO_HIGHEST+1]; /* human-readable call names */
EXTERN struct proc *kbill_kcall; /* process that made kernel call */
EXTERN struct proc *kbill_ipc; /* process that invoked ipc */

/* Interrupt related variables. */
EXTERN irq_hook_t irq_hooks[NR_IRQ_HOOKS];	/* hooks for general use */
EXTERN int irq_actids[NR_IRQ_VECTORS];		/* IRQ ID bits active */
EXTERN int irq_use;				/* map of all in-use irq's */

/* Miscellaneous. */
EXTERN int verboseboot;			/* verbose boot, init'ed in cstart */

#if DEBUG_TRACE
EXTERN int verboseflags;
#endif

#ifdef USE_APIC
EXTERN int config_no_apic; /* optionally turn off apic */
EXTERN int config_apic_timer_x; /* apic timer slowdown factor */
#endif

EXTERN u64_t cpu_hz[CONFIG_MAX_CPUS];

#define cpu_set_freq(cpu, freq)	do {cpu_hz[cpu] = freq;} while (0)
#define cpu_get_freq(cpu)	cpu_hz[cpu]

#ifdef CONFIG_SMP
EXTERN int config_no_smp; /* optionally turn off SMP */
#endif

/* VM */
EXTERN int vm_running;
EXTERN int catch_pagefaults;
EXTERN int kernel_may_alloc;

/* Variables that are initialized elsewhere are just extern here. */
extern struct boot_image image[NR_BOOT_PROCS]; 	/* system image processes */

EXTERN volatile int serial_debug_active;

EXTERN struct cpu_info cpu_info[CONFIG_MAX_CPUS];

/* BKL stats */
EXTERN u64_t kernel_ticks[CONFIG_MAX_CPUS];
EXTERN u64_t bkl_ticks[CONFIG_MAX_CPUS];
EXTERN unsigned bkl_tries[CONFIG_MAX_CPUS];
EXTERN unsigned bkl_succ[CONFIG_MAX_CPUS];

/* Feature flags */
EXTERN int minix_feature_flags;

/* Statistics about the entries into the kernel. */
struct kernel_entry_stats {
	unsigned tot_entries;
	struct {
		unsigned heatmap[NR_SYS_CALLS];
	} kernel_call_stats;

	struct {
		unsigned heatmap[SENDA+1];
	} ipc_call_stats;

	struct {
		unsigned heatmap[SIMD_EXCEPTION_VECTOR+1];
	} exception_stats;

	struct {
		unsigned heatmap[15+1]; /* 15 hw irqs. */
	} irq_stats;
};

EXTERN struct kernel_entry_stats kernel_entries_stats;
EXTERN int kernel_entries_reg_enable;
#define CHECK_KERNEL_ENTRIES_REG()					\
	do {								\
		if(!kernel_entries_reg_enable)				\
			return;						\
	} while(0)

static void add_to_heatmap(unsigned *heatmap,int idx,int low,int high)
{
	if((low)<=(idx)&&(idx)<=(high)) {
		heatmap[(idx)]++;
	} else {
		panic("Invalid idx in heatmap");
	}
}

#define ADD_TO_HEATMAP(s,idx,low,high) \
	add_to_heatmap(s.heatmap,idx,low,high)

#define KTRACE_SIZE 32768
SPINLOCK_DECLARE(ktrace_lock);
EXTERN struct kernel_trace_entry ktrace[KTRACE_SIZE];
EXTERN unsigned ktrace_idx;

static void reset_ktrace(void)
{
	/* This is just a hook for gdb, which will extract the data at this
	 * point. */
	/* Circular buffer. */
	ktrace_idx = 0;
}

static inline void add_ktrace(unsigned char event)
{
	spinlock_lock(&ktrace_lock);
	if(ktrace_idx==KTRACE_SIZE-1) {
		reset_ktrace();
	}
	/* Read the TSC reg and put it into the entry. */
	read_tsc_64(&(ktrace[ktrace_idx].timestamp));
	ktrace[ktrace_idx].cpu = cpuid;
	ktrace[ktrace_idx].event = event;
	ktrace_idx++;
	spinlock_unlock(&ktrace_lock);
}

static inline void reg_kernel_call(int call_nr)
{
	CHECK_KERNEL_ENTRIES_REG();
	kernel_entries_stats.tot_entries++;
	ADD_TO_HEATMAP(kernel_entries_stats.kernel_call_stats,call_nr,0,NR_SYS_CALLS-1);

	/* Kernel calls are indepotent mapped. */
	add_ktrace(call_nr);
}

static inline void reg_ipc_call(int call_nr)
{
	CHECK_KERNEL_ENTRIES_REG();
	kernel_entries_stats.tot_entries++;
	ADD_TO_HEATMAP(kernel_entries_stats.ipc_call_stats,call_nr,SEND,SENDA);

	if(call_nr==SENDA)
		add_ktrace(KTRACE_SENDA);
	else
		add_ktrace(KTRACE_EVENT_LOW+46-1+call_nr);
}

static inline void reg_exception(int exc_nr)
{
	CHECK_KERNEL_ENTRIES_REG();
	kernel_entries_stats.tot_entries++;
	ADD_TO_HEATMAP(kernel_entries_stats.exception_stats,exc_nr,DIVIDE_VECTOR,SIMD_EXCEPTION_VECTOR);
}

static inline void reg_irq(int irq_nr)
{
	CHECK_KERNEL_ENTRIES_REG();
	kernel_entries_stats.tot_entries++;
	ADD_TO_HEATMAP(kernel_entries_stats.irq_stats,irq_nr,0,15);
}
#endif /* GLO_H */

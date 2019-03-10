#include "kernel/watchdog.h"
#include "glo.h"
#include <minix/minlib.h>
#include <minix/u64.h>

#include "apic.h"

#define CPUID_UNHALTED_CORE_CYCLES_AVAILABLE	0

/*
 * Intel architecture performance counters watchdog
 */

static struct arch_watchdog intel_arch_watchdog;
static struct arch_watchdog amd_watchdog;

static void intel_arch_watchdog_init(const unsigned cpu)
{
	/* Nothing to do, the MSRs will be configured when starting the timer
	 * for the first time and upon every tick. */
}

static void intel_arch_watchdog_reinit(const unsigned cpu)
{
	/* We just recieved a tick, restart the timer with the frequency
	 * specified when it was first started. */
	const unsigned original_freq = watchdog->resetval;
	watchdog->profile_init(original_freq);
}

int arch_watchdog_init(void)
{
	u32_t eax, ebx, ecx, edx;
	unsigned cpu = cpuid;

	if (!lapic_addr) {
		printf("ERROR : Cannot use NMI watchdog if APIC is not enabled\n");
		return -1;
	}

	if (cpu_info[cpu].vendor == CPU_VENDOR_INTEL) {
		eax = 0xA;

		_cpuid(&eax, &ebx, &ecx, &edx);

		/* FIXME currently we support only watchdog based on the intel
		 * architectural performance counters. Some Intel CPUs don't have this
		 * feature
		 */
		if (ebx & (1 << CPUID_UNHALTED_CORE_CYCLES_AVAILABLE))
			return -1;
		if (!((((eax >> 8)) & 0xff) > 0))
			return -1;

		watchdog = &intel_arch_watchdog;
	} else if (cpu_info[cpu].vendor == CPU_VENDOR_AMD) {
		if (cpu_info[cpu].family != 6 &&
				cpu_info[cpu].family != 15 &&
				cpu_info[cpu].family != 16 &&
				cpu_info[cpu].family != 17)
			return -1;
		else
			watchdog = &amd_watchdog;
	} else
		return -1;

	/* Setup PC overflow as NMI for watchdog, it is masked for now */
	lapic_write(LAPIC_LVTPCR, APIC_ICR_INT_MASK | APIC_ICR_DM_NMI);
	(void) lapic_read(LAPIC_LVTPCR);

	/* double check if LAPIC is enabled */
	if (lapic_addr && watchdog->init) {
		watchdog->init(cpuid);
	}

	return 0;
}

void arch_watchdog_stop(void)
{
}

void arch_watchdog_lockup(const struct nmi_frame * frame)
{
	printf("KERNEL LOCK UP\n"
			"eax    0x%08x\n"
			"ecx    0x%08x\n"
			"edx    0x%08x\n"
			"ebx    0x%08x\n"
			"ebp    0x%08x\n"
			"esi    0x%08x\n"
			"edi    0x%08x\n"
			"gs     0x%08x\n"
			"fs     0x%08x\n"
			"es     0x%08x\n"
			"ds     0x%08x\n"
			"pc     0x%08x\n"
			"cs     0x%08x\n"
			"eflags 0x%08x\n",
			frame->eax,
			frame->ecx,
			frame->edx,
			frame->ebx,
			frame->ebp,
			frame->esi,
			frame->edi,
			frame->gs,
			frame->fs,
			frame->es,
			frame->ds,
			frame->pc,
			frame->cs,
			frame->eflags
			);
	panic("Kernel lockup");
}

int i386_watchdog_start(void)
{
	if (arch_watchdog_init()) {
		printf("WARNING watchdog initialization "
				"failed! Disabled\n");
		watchdog_enabled = 0;
		return -1;
	}
	else
		BOOT_VERBOSE(printf("Watchdog enabled\n"););

	return 0;
}

static int intel_arch_watchdog_profile_init(const unsigned freq)
{
	/* Start the timer with frequency `freq`. */

	/* Save the value of the freq for the reinit function. */
	watchdog->resetval = freq;

	/* Compute the original value of the counter. Assumes that all cpus
	 * have the same freq. TODO */
	const u64_t cpuf = cpu_get_freq(cpuid) / freq;

	/*
	 * if freq is too low and the cpu freq too high we may get in a range of
	 * insane value which cannot be handled by the 31bit CPU perf counter
	 */
	if (ex64hi(cpuf) != 0 || ex64lo(cpuf) > 0x7fffffffU) {
		printf("ERROR : nmi watchdog ticks exceed 31bits, use higher frequency\n");
		return EINVAL;
	}

	/* Reset the counter before changing the selector. */
	ia32_msr_write(INTEL_MSR_PERFMON_CRT0, 0, 0);

	/* Count `UnHalted Core Cycles` (3CH) in kernel and user-space.
	 * Also use LAPIC interrupt when the counter reaches 0. */
	const u32_t val = 1 << 20 | 1 << 17 | 1 << 16 | 0x3c;
	ia32_msr_write(INTEL_MSR_PERFMON_SEL0, 0, val);

	/* Configure the LAPIC to deliver an NMI when the counter reaches 0.
	 * We need to do this before starting the counter to avoid missing the
	 * first tick. */
	lapic_write(LAPIC_LVTPCR, APIC_ICR_DM_NMI);

	/* Init the value of the counter to the number of cycles before the
	 * next tick of the timer.
	 * Note that the counter is _not_ started at this point.
	 *
	 * Because the counter only count upwards we need to set the value
	 * to -1*cpuf.
	 */
	const u32_t counter_start = (u32_t)-ex64lo(cpuf);
	ia32_msr_write(INTEL_MSR_PERFMON_CRT0, 0, counter_start);

	/* Enable the counter. */
	const u32_t enabled_val = val | INTEL_MSR_PERFMON_SEL0_ENABLE;
	ia32_msr_write(INTEL_MSR_PERFMON_SEL0,0,enabled_val);

	return OK;
}

static struct arch_watchdog intel_arch_watchdog = {
	/*.init = */		intel_arch_watchdog_init,
	/*.reinit = */		intel_arch_watchdog_reinit,
	/*.profile_init = */	intel_arch_watchdog_profile_init
};

#define AMD_MSR_EVENT_SEL0		0xc0010000
#define AMD_MSR_EVENT_CTR0		0xc0010004
#define AMD_MSR_EVENT_SEL0_ENABLE	(1 << 22)

static void amd_watchdog_init(const unsigned cpu)
{
	u64_t cpuf;
	u32_t val;

	ia32_msr_write(AMD_MSR_EVENT_CTR0, 0, 0);

	/* Int, OS, USR, Cycles cpu is running */
	val = 1 << 20 | 1 << 17 | 1 << 16 | 0x76;
	ia32_msr_write(AMD_MSR_EVENT_SEL0, 0, val);

	cpuf = -cpu_get_freq(cpu);
	watchdog->resetval = watchdog->watchdog_resetval = cpuf;

	ia32_msr_write(AMD_MSR_EVENT_CTR0,
		       ex64hi(watchdog->resetval), ex64lo(watchdog->resetval));

	ia32_msr_write(AMD_MSR_EVENT_SEL0, 0,
			val | AMD_MSR_EVENT_SEL0_ENABLE);

	/* unmask the performance counter interrupt */
	lapic_write(LAPIC_LVTPCR, APIC_ICR_DM_NMI);
}

static void amd_watchdog_reinit(const unsigned cpu)
{
	lapic_write(LAPIC_LVTPCR, APIC_ICR_DM_NMI);
	ia32_msr_write(AMD_MSR_EVENT_CTR0,
		       ex64hi(watchdog->resetval), ex64lo(watchdog->resetval));
}

static int amd_watchdog_profile_init(const unsigned freq)
{
	u64_t cpuf;

	/* FIXME works only if all CPUs have the same freq */
	cpuf = cpu_get_freq(cpuid);
	cpuf = -cpuf / freq;

	watchdog->profile_resetval = cpuf;

	return OK;
}

static struct arch_watchdog amd_watchdog = {
	/*.init = */		amd_watchdog_init,
	/*.reinit = */		amd_watchdog_reinit,
	/*.profile_init = */	amd_watchdog_profile_init
};

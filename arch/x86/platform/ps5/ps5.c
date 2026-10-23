// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/ps5.h>

#include <asm/apic.h>
#include <asm/i8259.h>
#include <asm/reboot.h>
#include <asm/setup.h>

#define PS5_DEFAULT_TSC_FREQ 1596300000

static __noreturn void ps5_power_off(void) {
	icc_power_shutdown();
}

static __noreturn void ps5_emergency_restart(void) {
	icc_power_reboot();
}

static __noreturn void ps5_restart(char *cmd) {
	ps5_emergency_restart();
}

static unsigned long __init ps5_calibrate_tsc(void)
{
	return PS5_DEFAULT_TSC_FREQ / 1000;
}

static void ps5_get_wallclock(struct timespec64 *now)
{
	now->tv_sec = now->tv_nsec = 0;
}

static int ps5_set_wallclock(const struct timespec64 *now)
{
	return -ENODEV;
}

void __init x86_ps5_early_setup(void)
{
	x86_platform.calibrate_tsc = ps5_calibrate_tsc;
	x86_platform.get_wallclock = ps5_get_wallclock;
	x86_platform.set_wallclock = ps5_set_wallclock;
	x86_platform.legacy.rtc = 0;

	machine_ops.restart = ps5_restart;
	machine_ops.power_off = ps5_power_off;
	machine_ops.emergency_restart = ps5_emergency_restart;
	pm_power_off = ps5_power_off;

	legacy_pic = &null_legacy_pic;
}

/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2007-2009 coresystems GmbH
 * Copyright (C) 2014 Google Inc.
 * Copyright (C) 2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <console/console.h>
#include <device/device.h>
#include <device/pci.h>
#include <string.h>
#include <arch/acpi.h>
#include <chip.h>
#include <cpu/cpu.h>
#include <cpu/x86/mtrr.h>
#include <cpu/x86/msr.h>
#include <cpu/x86/lapic.h>
#include <cpu/x86/mp.h>
#include <cpu/intel/microcode.h>
#include <cpu/intel/speedstep.h>
#include <cpu/intel/turbo.h>
#include <cpu/x86/cache.h>
#include <cpu/x86/name.h>
#include <cpu/x86/smm.h>
#include <delay.h>
#include <pc80/mc146818rtc.h>
#include <soc/cpu.h>
#include <soc/msr.h>
#include <soc/pci_devs.h>
#include <soc/ramstage.h>
#include <soc/smm.h>
#include <soc/systemagent.h>

/* Convert time in seconds to POWER_LIMIT_1_TIME MSR value */
static const u8 power_limit_time_sec_to_msr[] = {
	[0]   = 0x00,
	[1]   = 0x0a,
	[2]   = 0x0b,
	[3]   = 0x4b,
	[4]   = 0x0c,
	[5]   = 0x2c,
	[6]   = 0x4c,
	[7]   = 0x6c,
	[8]   = 0x0d,
	[10]  = 0x2d,
	[12]  = 0x4d,
	[14]  = 0x6d,
	[16]  = 0x0e,
	[20]  = 0x2e,
	[24]  = 0x4e,
	[28]  = 0x6e,
	[32]  = 0x0f,
	[40]  = 0x2f,
	[48]  = 0x4f,
	[56]  = 0x6f,
	[64]  = 0x10,
	[80]  = 0x30,
	[96]  = 0x50,
	[112] = 0x70,
	[128] = 0x11,
};

/* Convert POWER_LIMIT_1_TIME MSR value to seconds */
static const u8 power_limit_time_msr_to_sec[] = {
	[0x00] = 0,
	[0x0a] = 1,
	[0x0b] = 2,
	[0x4b] = 3,
	[0x0c] = 4,
	[0x2c] = 5,
	[0x4c] = 6,
	[0x6c] = 7,
	[0x0d] = 8,
	[0x2d] = 10,
	[0x4d] = 12,
	[0x6d] = 14,
	[0x0e] = 16,
	[0x2e] = 20,
	[0x4e] = 24,
	[0x6e] = 28,
	[0x0f] = 32,
	[0x2f] = 40,
	[0x4f] = 48,
	[0x6f] = 56,
	[0x10] = 64,
	[0x30] = 80,
	[0x50] = 96,
	[0x70] = 112,
	[0x11] = 128,
};

/*
 * The core 100MHz BLCK is disabled in deeper c-states. One needs to calibrate
 * the 100MHz BCLCK against the 24MHz BLCK to restore the clocks properly
 * when a core is woken up.
 */
static int pcode_ready(void)
{
	int wait_count;
	const int delay_step = 10;

	wait_count = 0;
	do {
		if (!(MCHBAR32(BIOS_MAILBOX_INTERFACE) & MAILBOX_RUN_BUSY))
			return 0;
		wait_count += delay_step;
		udelay(delay_step);
	} while (wait_count < 1000);

	return -1;
}

static void calibrate_24mhz_bclk(void)
{
	int err_code;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on wait ready.\n");
		return;
	}

	/* A non-zero value initiates the PCODE calibration. */
	MCHBAR32(BIOS_MAILBOX_DATA) = ~0;
	MCHBAR32(BIOS_MAILBOX_INTERFACE) =
		MAILBOX_RUN_BUSY | MAILBOX_BIOS_CMD_FSM_MEASURE_INTVL;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on completion.\n");
		return;
	}

	err_code = MCHBAR32(BIOS_MAILBOX_INTERFACE) & 0xff;

	printk(BIOS_DEBUG, "PCODE: 24MHz BLCK calibration response: %d\n",
	       err_code);

	/* Read the calibrated value. */
	MCHBAR32(BIOS_MAILBOX_INTERFACE) =
		MAILBOX_RUN_BUSY | MAILBOX_BIOS_CMD_READ_CALIBRATION;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on read.\n");
		return;
	}

	printk(BIOS_DEBUG, "PCODE: 24MHz BLCK calibration value: 0x%08x\n",
	       MCHBAR32(BIOS_MAILBOX_DATA));
}

static void initialize_vr_config(void)
{
	msr_t msr;

	printk(BIOS_DEBUG, "Initializing VR config.\n");

	/*  Configure VR_CURRENT_CONFIG. */
	msr = rdmsr(MSR_VR_CURRENT_CONFIG);
	/*
	 * Preserve bits 63 and 62. Bit 62 is PSI4 enable, but it is only valid
	 * on ULT systems.
	 */
	msr.hi &= 0xc0000000;
	msr.hi |= (0x01 << (52 - 32)); /* PSI3 threshold -  1A. */
	msr.hi |= (0x05 << (42 - 32)); /* PSI2 threshold -  5A. */
	msr.hi |= (0x14 << (32 - 32)); /* PSI1 threshold - 20A. */
	msr.hi |= (1 <<  (62 - 32)); /* Enable PSI4 */
	/* Leave the max instantaneous current limit (12:0) to default. */
	wrmsr(MSR_VR_CURRENT_CONFIG, msr);

	/*  Configure VR_MISC_CONFIG MSR. */
	msr = rdmsr(MSR_VR_MISC_CONFIG);
	/* Set the IOUT_SLOPE scalar applied to dIout in U10.1.9 format. */
	msr.hi &= ~(0x3ff << (40 - 32));
	msr.hi |= (0x200 << (40 - 32)); /* 1.0 */
	/* Set IOUT_OFFSET to 0. */
	msr.hi &= ~0xff;
	/* Set exit ramp rate to fast. */
	msr.hi |= (1 << (50 - 32));
	/* Set entry ramp rate to slow. */
	msr.hi &= ~(1 << (51 - 32));
	/* Enable decay mode on C-state entry. */
	msr.hi |= (1 << (52 - 32));
	/* Set the slow ramp rate to be fast ramp rate / 4 */
	msr.hi &= ~(0x3 << (53 - 32));
	msr.hi |= (0x01 << (53 - 32));
	/* Set MIN_VID (31:24) to allow CPU to have full control. */
	msr.lo &= ~0xff000000;
	wrmsr(MSR_VR_MISC_CONFIG, msr);

	/*  Configure VR_MISC_CONFIG2 MSR. */
	msr = rdmsr(MSR_VR_MISC_CONFIG2);
	msr.lo &= ~0xffff;
	/*
	 * Allow CPU to control minimum voltage completely (15:8) and
	 * set the fast ramp voltage in 10mV steps.
	 */
	if (cpu_family_model() == SKYLAKE_FAMILY_ULT)
		msr.lo |= 0x006a; /* 1.56V */
	else
		msr.lo |= 0x006f; /* 1.60V */
	wrmsr(MSR_VR_MISC_CONFIG2, msr);
}

int cpu_config_tdp_levels(void)
{
	msr_t platform_info;

	/* Bits 34:33 indicate how many levels supported */
	platform_info = rdmsr(MSR_PLATFORM_INFO);
	return (platform_info.hi >> 1) & 3;
}

/*
 * Configure processor power limits if possible
 * This must be done AFTER set of BIOS_RESET_CPL
 */
void set_power_limits(u8 power_limit_1_time)
{
	msr_t msr = rdmsr(MSR_PLATFORM_INFO);
	msr_t limit;
	unsigned power_unit;
	unsigned tdp, min_power, max_power, max_time;
	u8 power_limit_1_val;

	if (power_limit_1_time > ARRAY_SIZE(power_limit_time_sec_to_msr))
		power_limit_1_time = 28;

	if (!(msr.lo & PLATFORM_INFO_SET_TDP))
		return;

	/* Get units */
	msr = rdmsr(MSR_PKG_POWER_SKU_UNIT);
	power_unit = 2 << ((msr.lo & 0xf) - 1);

	/* Get power defaults for this SKU */
	msr = rdmsr(MSR_PKG_POWER_SKU);
	tdp = msr.lo & 0x7fff;
	min_power = (msr.lo >> 16) & 0x7fff;
	max_power = msr.hi & 0x7fff;
	max_time = (msr.hi >> 16) & 0x7f;

	printk(BIOS_DEBUG, "CPU TDP: %u Watts\n", tdp / power_unit);

	if (power_limit_time_msr_to_sec[max_time] > power_limit_1_time)
		power_limit_1_time = power_limit_time_msr_to_sec[max_time];

	if (min_power > 0 && tdp < min_power)
		tdp = min_power;

	if (max_power > 0 && tdp > max_power)
		tdp = max_power;

	power_limit_1_val = power_limit_time_sec_to_msr[power_limit_1_time];

	/* Set long term power limit to TDP */
	limit.lo = 0;
	limit.lo |= tdp & PKG_POWER_LIMIT_MASK;
	limit.lo |= PKG_POWER_LIMIT_EN;
	limit.lo |= (power_limit_1_val & PKG_POWER_LIMIT_TIME_MASK) <<
		PKG_POWER_LIMIT_TIME_SHIFT;

	/* Set short term power limit to 1.25 * TDP */
	limit.hi = 0;
	limit.hi |= ((tdp * 125) / 100) & PKG_POWER_LIMIT_MASK;
	limit.hi |= PKG_POWER_LIMIT_EN;

	/* Power limit 2 time is only programmable on server SKU */
	wrmsr(MSR_PKG_POWER_LIMIT, limit);

	/* Set power limit values in MCHBAR as well */
	MCHBAR32(MCH_PKG_POWER_LIMIT_LO) = limit.lo;
	MCHBAR32(MCH_PKG_POWER_LIMIT_HI) = limit.hi;

	/* Set DDR RAPL power limit by copying from MMIO to MSR */
	msr.lo = MCHBAR32(MCH_DDR_POWER_LIMIT_LO);
	msr.hi = MCHBAR32(MCH_DDR_POWER_LIMIT_HI);
	wrmsr(MSR_DDR_RAPL_LIMIT, msr);

	/* Use nominal TDP values for CPUs with configurable TDP */
	if (cpu_config_tdp_levels()) {
		msr = rdmsr(MSR_CONFIG_TDP_NOMINAL);
		limit.hi = 0;
		limit.lo = msr.lo & 0xff;
		wrmsr(MSR_TURBO_ACTIVATION_RATIO, limit);
	}
}

static void configure_thermal_target(void)
{
	device_t dev = SA_DEV_ROOT;
	config_t *conf = dev->chip_info;
	msr_t msr;

	/* Set TCC activaiton offset if supported */
	msr = rdmsr(MSR_PLATFORM_INFO);
	if ((msr.lo & (1 << 30)) && conf->tcc_offset) {
		msr = rdmsr(MSR_TEMPERATURE_TARGET);
		msr.lo &= ~(0xf << 24); /* Bits 27:24 */
		msr.lo |= (conf->tcc_offset & 0xf) << 24;
		wrmsr(MSR_TEMPERATURE_TARGET, msr);
	}
}

static void configure_misc(void)
{
	msr_t msr;

	msr = rdmsr(IA32_MISC_ENABLE);
	msr.lo |= (1 << 0);	/* Fast String enable */
	msr.lo |= (1 << 3);	/* TM1/TM2/EMTTM enable */
	msr.lo |= (1 << 16);	/* Enhanced SpeedStep Enable */
	wrmsr(IA32_MISC_ENABLE, msr);

	/* Disable Thermal interrupts */
	msr.lo = 0;
	msr.hi = 0;
	wrmsr(IA32_THERM_INTERRUPT, msr);

	/* Enable package critical interrupt only */
	msr.lo = 1 << 4;
	msr.hi = 0;
	wrmsr(IA32_PACKAGE_THERM_INTERRUPT, msr);
}

static void enable_lapic_tpr(void)
{
	msr_t msr;

	msr = rdmsr(MSR_PIC_MSG_CONTROL);
	msr.lo &= ~(1 << 10);	/* Enable APIC TPR updates */
	wrmsr(MSR_PIC_MSG_CONTROL, msr);
}

static void configure_dca_cap(void)
{
	struct cpuid_result cpuid_regs;
	msr_t msr;

	/* Check feature flag in CPUID.(EAX=1):ECX[18]==1 */
	cpuid_regs = cpuid(1);
	if (cpuid_regs.ecx & (1 << 18)) {
		msr = rdmsr(IA32_PLATFORM_DCA_CAP);
		msr.lo |= 1;
		wrmsr(IA32_PLATFORM_DCA_CAP, msr);
	}
}

static void set_max_ratio(void)
{
	msr_t msr, perf_ctl;

	perf_ctl.hi = 0;

	/* Check for configurable TDP option */
	if (get_turbo_state() == TURBO_ENABLED) {
		msr = rdmsr(MSR_TURBO_RATIO_LIMIT);
		perf_ctl.lo = (msr.lo & 0xff) << 8;
	} else if (cpu_config_tdp_levels()) {
		/* Set to nominal TDP ratio */
		msr = rdmsr(MSR_CONFIG_TDP_NOMINAL);
		perf_ctl.lo = (msr.lo & 0xff) << 8;
	} else {
		/* Platform Info bits 15:8 give max ratio */
		msr = rdmsr(MSR_PLATFORM_INFO);
		perf_ctl.lo = msr.lo & 0xff00;
	}
	wrmsr(IA32_PERF_CTL, perf_ctl);

	printk(BIOS_DEBUG, "cpu: frequency set to %d\n",
	       ((perf_ctl.lo >> 8) & 0xff) * CPU_BCLK);
}

static void set_energy_perf_bias(u8 policy)
{
	msr_t msr;
	int ecx;

	/* Determine if energy efficient policy is supported. */
	ecx = cpuid_ecx(0x6);
	if (!(ecx & (1 << 3)))
		return;

	/* Energy Policy is bits 3:0 */
	msr = rdmsr(IA32_ENERGY_PERFORMANCE_BIAS);
	msr.lo &= ~0xf;
	msr.lo |= policy & 0xf;
	wrmsr(IA32_ENERGY_PERFORMANCE_BIAS, msr);

	printk(BIOS_DEBUG, "cpu: energy policy set to %u\n", policy);
}

static void configure_mca(void)
{
	msr_t msr;
	const unsigned int mcg_cap_msr = 0x179;
	int i;
	int num_banks;

	msr = rdmsr(mcg_cap_msr);
	num_banks = msr.lo & 0xff;
	msr.lo = msr.hi = 0;
	/*
	 * TODO(adurbin): This should only be done on a cold boot. Also, some
	 * of these banks are core vs package scope. For now every CPU clears
	 * every bank.
	 */
	for (i = 0; i < num_banks; i++)
		wrmsr(IA32_MC0_STATUS + (i * 4), msr);
}

static void bsp_init_before_ap_bringup(struct bus *cpu_bus)
{
	/* Setup MTRRs based on physical address size. */
	x86_setup_fixed_mtrrs();
	x86_setup_var_mtrrs(cpuid_eax(0x80000008) & 0xff, 2);
	x86_mtrr_check();

	initialize_vr_config();
	calibrate_24mhz_bclk();
}

/* All CPUs including BSP will run the following function. */
static void cpu_core_init(device_t cpu)
{
	/* Clear out pending MCEs */
	configure_mca();

	/* Enable the local cpu apics */
	enable_lapic_tpr();
	setup_lapic();

	/* Configure Enhanced SpeedStep and Thermal Sensors */
	configure_misc();

	/* Thermal throttle activation offset */
	configure_thermal_target();

	/* Enable Direct Cache Access */
	configure_dca_cap();

	/* Set energy policy */
	set_energy_perf_bias(ENERGY_POLICY_NORMAL);

	/* Enable Turbo */
	enable_turbo();
}

/* MP initialization support. */
static const void *microcode_patch;
int ht_disabled;

static int adjust_apic_id_ht_disabled(int index, int apic_id)
{
	return 2 * index;
}

static void relocate_and_load_microcode(void *unused)
{
	/* Relocate the SMM handler. */
	smm_relocate();

	/* After SMM relocation a 2nd microcode load is required. */
	intel_microcode_load_unlocked(microcode_patch);
}

static void enable_smis(void *unused)
{
	/*
	 * Now that all APs have been relocated as well as the BSP let SMIs
	 * start flowing.
	 */
	southbridge_smm_enable_smi();

	/* Lock down the SMRAM space. */
#if IS_ENABLED(CONFIG_HAVE_SMI_HANDLER)
	smm_lock();
#endif
}

static struct mp_flight_record mp_steps[] = {
	MP_FR_NOBLOCK_APS(relocate_and_load_microcode, NULL,
			  relocate_and_load_microcode, NULL),
#if IS_ENABLED(CONFIG_SMP)
	MP_FR_BLOCK_APS(mp_initialize_cpu, NULL, mp_initialize_cpu, NULL),
	/* Wait for APs to finish initialization before proceeding. */
#endif
	MP_FR_BLOCK_APS(NULL, NULL, enable_smis, NULL),
};

static struct device_operations cpu_dev_ops = {
	.init = cpu_core_init,
	.acpi_fill_ssdt_generator = generate_cpu_entries,
};

static struct cpu_device_id cpu_table[] = {
	{ X86_VENDOR_INTEL, CPUID_SKYLAKE_C0 },
	{ X86_VENDOR_INTEL, CPUID_SKYLAKE_D0 },
	{ 0, 0 },
};

static const struct cpu_driver driver __cpu_driver = {
	.ops      = &cpu_dev_ops,
	.id_table = cpu_table,
};

void soc_init_cpus(device_t dev)
{
	struct bus *cpu_bus = dev->link_list;
	int num_threads;
	int num_cores;
	msr_t msr;
	struct mp_params mp_params;
	void *smm_save_area;

	msr = rdmsr(CORE_THREAD_COUNT_MSR);
	num_threads = (msr.lo >> 0) & 0xffff;
	num_cores = (msr.lo >> 16) & 0xffff;
	printk(BIOS_DEBUG, "CPU has %u cores, %u threads enabled.\n",
	       num_cores, num_threads);

	ht_disabled = num_threads == num_cores;

	/*
	 * Perform any necessary BSP initialization before APs are brought up.
	 * This call also allows the BSP to prepare for any secondary effects
	 * from calling cpu_initialize() such as smm_init().
	 */
	bsp_init_before_ap_bringup(cpu_bus);

	microcode_patch = intel_microcode_find();

	/* Save default SMM area before relocation occurs. */
	if (IS_ENABLED(CONFIG_HAVE_SMI_HANDLER))
		smm_save_area = backup_default_smm_area();
	else
		smm_save_area = NULL;

	mp_params.num_cpus = num_threads;
	mp_params.parallel_microcode_load = 1;
	if (ht_disabled)
		mp_params.adjust_apic_id = adjust_apic_id_ht_disabled;
	else
		mp_params.adjust_apic_id = NULL;
	mp_params.flight_plan = &mp_steps[0];
	mp_params.num_records = ARRAY_SIZE(mp_steps);
	mp_params.microcode_pointer = microcode_patch;

	/* Load relocation and permeanent handlers. Then initiate relocation. */
	if (smm_initialize())
		printk(BIOS_CRIT, "SMM Initialiazation failed...\n");

	if (IS_ENABLED(CONFIG_SMP))
		if (mp_init(cpu_bus, &mp_params))
			printk(BIOS_ERR, "MP initialization failure.\n");

	/* Set Max Ratio */
	set_max_ratio();

	/* Restore the default SMM region. */
	if (IS_ENABLED(CONFIG_HAVE_SMI_HANDLER))
		restore_default_smm_area(smm_save_area);
}

int soc_skip_ucode_update(u32 current_patch_id, u32 new_patch_id)
{
	msr_t msr;
	/* If PRMRR/SGX is supported the FIT microcode load will set the msr
	 * 0x08b with the Patch revision id one less than the id in the
	 * microcode binary. The PRMRR support is indicated in the MSR
	 * MTRRCAP[12]. Check for this feature and avoid reloading the
	 * same microcode during cpu initialization.
	 */
	msr = rdmsr(MTRRcap_MSR);
	return (msr.lo & PRMRR_SUPPORTED) && (current_patch_id == new_patch_id - 1);
}

/* Copyright (C) 2026 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https: //alifsemi.com/license
 *
 */

/*
 * Deep-sleep wakeup from the low-power comparator (LPCMP).
 *
 * Which deep state is demonstrated depends on where the image boots from,
 * exactly as in the plain system_off sample:
 *
 *   - TCM boot (VTOR = 0): PM_STATE_SUSPEND_TO_RAM, first the STANDBY
 *     substate then the STOP substate. Context is retained, so the sample
 *     can report which source woke it - the LPCMP, or the LPRTC timeout.
 *
 *   - MRAM boot (VTOR >= 0x80000000): PM_STATE_SOFT_OFF. There is no
 *     retention, so a wakeup resets the subsystem and execution restarts
 *     from main(). Whether the LPCMP or the LPRTC woke it is visible from
 *     how long the board stayed dark.
 *
 * Either way the wakeup sources are the same: LPCMP as the primary source
 * and LPRTC as the timeout/backup source.
 *
 * The two SUSPEND_TO_RAM off profiles come from devicetree - see the
 * pm-system-off-lpcmp-he snippet, which adds ALIF_WE_LPCMP to
 * wakeup-events and ALIF_EWIC_VBAT_LP_CMP_IRQ to ewic-cfg on top of the
 * LPRTC wakeup pm-system-off-he already configures.
 *
 * SOFT_OFF is handled here in the application instead. pm-system-off-he
 * pins the shared aipm-off vtor-address to 0x0 for the suspend-to-RAM
 * resume path, which is the wrong entry point for an MRAM boot, so this
 * sample supplies that profile at runtime with the live SCB->VTOR.
 */

#include "aipm.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/comparator.h>
#include <se_service.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pm_lpcmp, LOG_LEVEL_INF);

#if !defined(CONFIG_RTSS_HE)
#error "The LP comparator wakeup path is an RTSS-HE feature"
#endif

#if !defined(CONFIG_ALIF_SE_DTS_OFF_PROFILE) || !defined(CONFIG_ALIF_SE_DTS_RUN_PROFILE)
#error "Build with -S pm-system-off-lpcmp-he so the AIPM run/off profiles come from DTS"
#endif

#define LPCMP_NODE DT_NODELABEL(lpcmp)

#if !DT_NODE_HAS_STATUS(LPCMP_NODE, okay)
#error "lpcmp is disabled - build with -S pm-system-off-lpcmp-he"
#endif

#define LPCMP_IRQN DT_IRQN(LPCMP_NODE)

/* Wakeup sources, kept identical across every deep state this sample enters */
#define APP_WAKEUP_EVENTS (WE_LPCMP | WE_LPRTC)
#define APP_EWIC_CFG      (EWIC_VBAT_LP_CMP_IRQ | EWIC_RTC_A)

/*
 * Sleep budgets, chosen against the min-residency-us values the
 * pm-system-off-he snippet installs:
 *   standby_s2ram 19 s, stop_s2ram 21 s, subsys_off 25 s.
 * So 20 s selects the STANDBY substate, 22 s the STOP substate, and 26 s
 * reaches SOFT_OFF.
 */
#define S2RAM_STANDBY_SLEEP_MS 20000
#define S2RAM_STOP_SLEEP_MS    22000
#define SOFT_OFF_SLEEP_MS      26000

/* Short RUNTIME_IDLE nap before the first deep sleep, so the console settles */
#define RUNTIME_IDLE_SLEEP_MS  2000

/*
 * The comparator line is level-asserted for as long as the input sits across
 * the reference, so arming it in that state just fires the ISR immediately.
 * Probe for that before every sleep, and retry on this cadence until the
 * input is quiet again.
 */
#define LPCMP_PROBE_MS   100
#define LPCMP_BACKOFF_MS 2000

/*
 * MRAM base address - used to determine boot location.
 * TCM boot: VTOR = 0x0, MRAM boot: VTOR >= 0x80000000.
 * Only a TCM boot retains code and context across suspend-to-RAM; an MRAM
 * boot uses SOFT_OFF and resets on wakeup.
 */
#define MRAM_BASE_ADDRESS 0x80000000
#define IS_BOOTING_FROM_MRAM() (SCB->VTOR >= MRAM_BASE_ADDRESS)

#define S2RAM_SUPPORTED    (!IS_BOOTING_FROM_MRAM())
#define SOFT_OFF_SUPPORTED IS_BOOTING_FROM_MRAM()

BUILD_ASSERT(S2RAM_STOP_SLEEP_MS > S2RAM_STANDBY_SLEEP_MS,
	"STOP sleep duration should be greater than STANDBY sleep duration");
BUILD_ASSERT(SOFT_OFF_SLEEP_MS > S2RAM_STOP_SLEEP_MS,
	"SOFT_OFF sleep duration should be greater than STOP sleep duration");

static const struct device *const lpcmp_dev = DEVICE_DT_GET(LPCMP_NODE);

/* Given from the LPCMP ISR, taken by main to tell the two wake causes apart */
static K_SEM_DEFINE(lpcmp_wake_sem, 0, 1);

/**
 * LPCMP trigger callback - runs in interrupt context.
 *
 * Masking the IRQ here is not an optimisation, it is what makes the sample
 * boot at all. The LP comparator drives its NVIC line straight from the
 * comparator output and has no interrupt-status register to acknowledge (its
 * register block is just LPCOMP_CTRL in the VBAT domain), so the line stays
 * asserted for as long as the input sits across the reference. Masking it on
 * the first ISR and re-arming from lpcmp_arm_for_sleep() before each sleep is
 * only way to stop it re-entering forever.
 */
static void lpcmp_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	irq_disable(LPCMP_IRQN);
	k_sem_give(&lpcmp_wake_sem);
}

/**
 * Bring up the LP comparator and enable it.
 *
 * For the LP instance, comparator_set_trigger() is what actually writes the
 * terminal/hysteresis selection into VBAT_ANA_REG2 and enables the block;
 * the trigger kind itself is only honoured by the high-speed instances.
 */
static int lpcmp_arm(void)
{
	int ret;

	if (!device_is_ready(lpcmp_dev)) {
		LOG_ERR("%s: device not ready", lpcmp_dev->name);
		return -ENODEV;
	}

	/* The callback was already registered in app_pre_kernel_init() */
	ret = comparator_set_trigger(lpcmp_dev, COMPARATOR_TRIGGER_BOTH_EDGES);
	if (ret) {
		LOG_ERR("Could not enable LPCMP (err %d)", ret);
		return ret;
	}

	/*
	 * The SE side is already taken care of: because the lpcmp node is
	 * enabled, soc_lpcmp_init() in the SoC layer has run at
	 * COMPARATOR_INIT_PRIORITY and pushed this same terminal selection to
	 * the SE with se_service_configure_lpcmp(), after turning the analog
	 * peripheral supply on. Nothing to duplicate here.
	 */
	LOG_INF("LPCMP armed (pos %u, neg %u, hyst %u)",
		DT_ENUM_IDX(LPCMP_NODE, positive_input),
		DT_ENUM_IDX(LPCMP_NODE, negative_input),
		DT_ENUM_IDX(LPCMP_NODE, hysteresis_level));

	return 0;
}

/* Drop any stale event and unmask the LPCMP IRQ */
static void lpcmp_unmask(void)
{
	k_sem_reset(&lpcmp_wake_sem);
	NVIC_ClearPendingIRQ(LPCMP_IRQN);
	irq_enable(LPCMP_IRQN);
}

/**
 * Arm the LPCMP for a sleep, waiting out an input parked across the reference.
 *
 * Because the line is level-asserted, unmasking it while the input is already
 * across the reference fires the ISR at once and masks it straight back:
 * sleeping then would just wake instantly. Probe for that, and if it happens,
 * wait and retry until the input is quiet. On return the IRQ is unmasked and
 * has stayed quiet for a full probe window, so the caller can sleep on it.
 *
 * Deeper power states are locked for the duration. The comparator is not
 * usable as a wake source while we are still probing it, so entering one here
 * could park the board with nothing to bring it back. That leaves
 * RUNTIME_IDLE - the plain WFI this sample already uses at start-up.
 */
static void lpcmp_arm_for_sleep(void)
{
	bool parked = false;

	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	while (true) {
		lpcmp_unmask();

		if (k_sem_take(&lpcmp_wake_sem, K_MSEC(LPCMP_PROBE_MS)) != 0) {
			/* Stayed quiet for the whole probe window */
			break;
		}

		if (!parked) {
			LOG_WRN("LPCMP input is parked across the reference - "
				"waiting for it to clear");
			parked = true;
		}

		k_sleep(K_MSEC(LPCMP_BACKOFF_MS));
	}

	if (parked) {
		LOG_INF("LPCMP input cleared");
	}

	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
}

/**
 * Supply the SOFT_OFF off profile.
 *
 * The DTS off profiles installed by pm-system-off-lpcmp-he only cover the
 * two SUSPEND_TO_RAM substates: their shared aipm-off vtor-address is 0x0,
 * which is the TCM resume entry point and wrong for an MRAM boot. So the
 * SOFT_OFF profile is built here instead, with the live SCB->VTOR, and the
 * DTS handler skips the state because off_profile_soft_off stays disabled.
 */
static void app_set_soft_off_params(void)
{
	off_profile_t offp = {
		.power_domains   = PD_VBAT_AON_MASK,
		.dcdc_voltage    = 825,
		.dcdc_mode       = DCDC_MODE_OFF,
		.aon_clk_src     = CLK_SRC_LFXO,
		.stby_clk_src    = CLK_SRC_HFRC,
		.stby_clk_freq   = SCALED_FREQ_RC_STDBY_76_8_MHZ,
		.memory_blocks   = MRAM_MASK | SERAM_MASK,
		.ip_clock_gating = 0,
		.phy_pwr_gating  = 0,
		.vdd_ioflex_3V3  = IOFLEX_LEVEL_1V8,
		.wakeup_events   = APP_WAKEUP_EVENTS,
		.ewic_cfg        = APP_EWIC_CFG,
		.vtor_address    = SCB->VTOR,
		.vtor_address_ns = 0,
	};
	int ret = se_service_set_off_cfg(&offp);

	if (ret) {
		LOG_ERR("SE: set_off_cfg for SOFT_OFF failed (err %d)", ret);
	}
}

static void app_pm_notify_state_entry(enum pm_state state)
{
	if (state == PM_STATE_SOFT_OFF) {
		app_set_soft_off_params();
	}
}

static struct pm_notifier app_pm_notifier = {
	.state_entry = app_pm_notify_state_entry,
};

/*
 * Runs in PRE_KERNEL_2, before main(): hold every deep state down until the
 * comparator has actually been armed, hook up the SOFT_OFF profile, and -
 * critically - register the LPCMP callback before the comparator driver can
 * unmask the interrupt.
 *
 * comparator_alif_comp.c unmasks the LP line unconditionally at the end of
 * its POST_KERNEL init, and its ISR does nothing for the LP instance except
 * invoke data->callback. Right after an LPCMP wakeup the comparator is still
 * enabled (VBAT is retained) and the input is still across the reference, so
 * that line is already asserted when the driver unmasks it. With no callback
 * registered there is nothing to mask it and the boot livelocks - which also
 * starves the SE/MHU round trips that soc_lpcmp_init() makes a few init
 * entries later, at the same priority. Registering the callback here means
 * the very first ISR masks the line and boot proceeds normally.
 *
 * comparator_set_trigger_callback() only stores the function pointer in the
 * driver's data struct, so it is safe to call before the device's own init
 * has run; the hardware setup still happens later, from lpcmp_arm().
 */
static int app_pre_kernel_init(void)
{
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	/*
	 * SUSPEND_TO_IDLE is locked for the lifetime of the sample. The deep
	 * states here are selected by long timeouts; the only sleeps short
	 * enough to land on SUSPEND_TO_IDLE are the internal waits, and those
	 * should stay on a plain WFI.
	 */
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);

	pm_notifier_register(&app_pm_notifier);

	(void)comparator_set_trigger_callback(lpcmp_dev, lpcmp_callback, NULL);

	return 0;
}
SYS_INIT(app_pre_kernel_init, PRE_KERNEL_2, 0);

/*
 * Belt and braces for the case where the comparator input is *not* across the
 * reference at boot: no ISR fires, so nothing masks the line the driver just
 * unmasked. Mask it here so the sample owns the enable in every case.
 */
static int lpcmp_mask_after_driver_init(void)
{
	irq_disable(LPCMP_IRQN);

	return 0;
}
SYS_INIT(lpcmp_mask_after_driver_init, POST_KERNEL,
	 UTIL_INC(CONFIG_COMPARATOR_INIT_PRIORITY));

/**
 * Sleep until either the LPCMP fires or the timeout expires, and report which.
 *
 * The idle thread is what actually enters the deep state; the timeout passed
 * here is the residency the PM policy sees, and therefore picks the state.
 */
static void app_deep_sleep(const char *label, uint32_t timeout_ms)
{
	int64_t start, elapsed;
	int ret;

	lpcmp_arm_for_sleep();

	LOG_INF("Entering %s for up to %u ms", label, timeout_ms);

	start = k_uptime_get();
	ret = k_sem_take(&lpcmp_wake_sem, K_MSEC(timeout_ms));
	elapsed = k_uptime_get() - start;

	if (ret == 0) {
		LOG_INF("=== Resumed from %s: woken by LPCMP after %lld ms ===",
			label, elapsed);
	} else {
		LOG_INF("=== Resumed from %s: woken by LPRTC timeout after %lld ms ===",
			label, elapsed);
	}

	/*
	 * Note: comparator_get_output() is not usable on the LP instance - it
	 * reads CMP_STATUS at offset 0x18, which is past the end of the LP
	 * comparator's 16-byte register block (LPCOMP_CTRL only). The wake
	 * cause above is the reliable indication.
	 */
}

/* MRAM boot: SOFT_OFF resets on wakeup, so this normally does not return */
static void app_soft_off(void)
{
	int ret;

	lpcmp_arm_for_sleep();

	LOG_INF("Entering PM_STATE_SOFT_OFF (no retention - wakeup resets the "
		"subsystem), LPRTC backup in %u ms", SOFT_OFF_SLEEP_MS);

	ret = k_sem_take(&lpcmp_wake_sem, K_MSEC(SOFT_OFF_SLEEP_MS));

	/*
	 * Getting here means the subsystem never went off: either the
	 * comparator tripped while the CPU was still awake, or the PM policy
	 * declined the state.
	 */
	if (ret == 0) {
		LOG_INF("LPCMP tripped before SOFT_OFF was entered");
	} else {
		LOG_WRN("Timeout expired without entering SOFT_OFF - "
			"is a debugger attached?");
	}
}

int main(void)
{
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	int round = 0;

	__ASSERT(device_is_ready(cons), "%s: device not ready", cons->name);

	LOG_INF("%s RTSS_HE (%s boot): LPCMP deep-sleep wakeup demo",
		CONFIG_BOARD, IS_BOOTING_FROM_MRAM() ? "MRAM" : "TCM");
	LOG_INF("Deep state: %s",
		S2RAM_SUPPORTED ? "PM_STATE_SUSPEND_TO_RAM (STANDBY, then STOP)"
				: "PM_STATE_SOFT_OFF (resets on wakeup)");
	LOG_INF("Wakeup sources: LPCMP (primary) and LPRTC (timeout/backup)");
	LOG_INF("Drive the LPCMP positive terminal (P2_4) across ~0.8 V to wake");

	if (lpcmp_arm()) {
		return -EIO;
	}

	/* Settle the console before the first deep sleep */
	k_sleep(K_MSEC(RUNTIME_IDLE_SLEEP_MS));

	/* From here on the idle thread may take the subsystem down */
	if (S2RAM_SUPPORTED) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
	} else {
		pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	}

	while (CONFIG_APP_LPCMP_WAKEUP_ROUNDS == 0 ||
	       round < CONFIG_APP_LPCMP_WAKEUP_ROUNDS) {
		round++;
		LOG_INF("--- round %d ---", round);

		if (S2RAM_SUPPORTED) {
			app_deep_sleep("PM_STATE_SUSPEND_TO_RAM (substate 0: STANDBY)",
				       S2RAM_STANDBY_SLEEP_MS);
			app_deep_sleep("PM_STATE_SUSPEND_TO_RAM (substate 1: STOP)",
				       S2RAM_STOP_SLEEP_MS);
		} else {
			app_soft_off();
		}
	}

	LOG_INF("=== POWER STATE SEQUENCE COMPLETED ===");
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	while (true) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}

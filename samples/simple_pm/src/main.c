/* Copyright (C) Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/uart.h>
#include <cmsis_core.h>
#include <soc_common.h>
#include <se_service.h>

#define DEBUG_PIN_NODE DT_ALIAS(debug_pin)

#if DT_NODE_EXISTS(DEBUG_PIN_NODE)
static const struct gpio_dt_spec debug_pin = GPIO_DT_SPEC_GET_OR(DEBUG_PIN_NODE, gpios, {0});
#endif

/**
 * As per the application requirements, it can remove the memory blocks which are not in use.
 */
#define RET_A1 SRAM4_1_MASK
#define RET_A2 SRAM5_1_MASK
#define RET_B  (SRAM4_2_MASK | SRAM5_2_MASK)
#define RET_C  (SRAM4_3_MASK | SRAM5_3_MASK)
#define RET_D  (SRAM4_4_MASK | SRAM5_4_MASK)
#define RET_E  (SRAM5_5_MASK)

#if 1 /* All retention lines enabled by default */
#define APP_RET_MEM_BLOCKS (RET_A1 | RET_A2 | RET_B | RET_C | RET_D | RET_E)
#else
#define APP_RET_MEM_BLOCKS  RET_A2 | RET_B
#endif

#define SERAM_MEMORY_BLOCKS_IN_USE SERAM_1_MASK | SERAM_2_MASK | SERAM_3_MASK | SERAM_4_MASK

#define OFF_HFRC 1
#define RUN_HFRC 1

#define LPGPIO_NODE           DT_NODELABEL(lpgpio)
#define LPGPIO_WAKEUP_ENABLED DT_NODE_HAS_STATUS_OKAY(LPGPIO_NODE)

#define LPCMP_NODE    DT_NODELABEL(lpcmp)
#define LPCMP_ENABLED DT_NODE_HAS_STATUS_OKAY(LPCMP_NODE)

#if LPGPIO_WAKEUP_ENABLED
/* LPGPIO wake up source is used */
#define LPGPIO_EWIC_CFG EWIC_VBAT_GPIO
#if CONFIG_LPGPIO_WAKEUP_SOURCE == 1
#define LPGPIO_WAKEUP_EVENT WE_LPGPIO1
#else
#define LPGPIO_WAKEUP_EVENT WE_LPGPIO0
#endif
#else
#define LPGPIO_EWIC_CFG 0
#define LPGPIO_WAKEUP_EVENT 0
#endif

#if LPCMP_ENABLED
/* LPCMP wake up source is used */
#define LPCMP_EWIC_CFG EWIC_VBAT_LP_CMP_IRQ
#define LPCMP_WAKEUP_EVENT WE_LPCMP
#else
#define LPCMP_EWIC_CFG 0
#define LPCMP_WAKEUP_EVENT 0
#endif

/* Is this ok?
 * #define WAKEUP_SOURCE DT_CHOSEN(zephyr_cortex_m_idle_timer)
 */

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(rtc0), snps_dw_apb_rtc, okay)
#define WAKEUP_SOURCE         DT_NODELABEL(rtc0)
#define SE_OFFP_EWIC_CFG      EWIC_RTC_A
#define SE_OFFP_WAKEUP_EVENTS WE_LPRTC
#elif DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(timer0), snps_dw_timers, okay)
#define WAKEUP_SOURCE         DT_NODELABEL(timer0)
#define SE_OFFP_EWIC_CFG      EWIC_VBAT_TIMER
#define SE_OFFP_WAKEUP_EVENTS WE_LPTIMER0
#else
#error "Wakeup Device not enabled in the dts"
#endif

#define WAKEUP_SOURCE_IRQ DT_IRQ_BY_IDX(WAKEUP_SOURCE, 0, irq)

#define RTC_WAKEUP_INTERVAL_MS           CONFIG_SLEEP_TIME_DISCONNECTED
#define RTC_CONNECTED_WAKEUP_INTERVAL_MS CONFIG_SLEEP_TIME_CONNECTED
#define SERVICE_INTERVAL_MS              RTC_CONNECTED_WAKEUP_INTERVAL_MS

#if LPGPIO_WAKEUP_ENABLED
static const struct gpio_dt_spec lpgpio_config = GPIO_DT_SPEC_GET_BY_IDX_OR(
	DT_NODELABEL(wakeup_pins), lpgpios, CONFIG_LPGPIO_WAKEUP_SOURCE, {0});
#endif

enum pm_state_mode_type {
	PM_STATE_MODE_IDLE,
	PM_STATE_MODE_STANDBY,
	PM_STATE_MODE_STOP
};

enum wakeup_status {
	WAKEUP_COLD = 0,
	WAKEUP_TIMER = 1 << 0,
	WAKEUP_LPGPIO = 1 << 1,
	WAKEUP_LPCMP = 1 << 2,
};

static volatile uint32_t wakeup_status = WAKEUP_COLD; /**< \ref enum wakeup_status */
static volatile int run_profile_error;

K_SEM_DEFINE(button_wait_sem, 0, 1);

/* Macros */
LOG_MODULE_REGISTER(main, CONFIG_MAIN_LOG_LEVEL);

static int set_off_profile(enum pm_state_mode_type const pm_mode)
{
	int ret;
	off_profile_t offp;

	/* Set default for stop mode with RTC wakeup support */
	offp.power_domains = PD_VBAT_AON_MASK;
/* If CONFIG_FLASH_BASE_ADDRESS is zero application run from itcm and no MRAM needed */
#if (CONFIG_FLASH_BASE_ADDRESS == 0)
	offp.memory_blocks = 0;
#else
	offp.memory_blocks = MRAM_MASK;
#endif
	offp.memory_blocks |= SERAM_MEMORY_BLOCKS_IN_USE;
	offp.memory_blocks |= APP_RET_MEM_BLOCKS;
	offp.dcdc_voltage = 775;

	switch (pm_mode) {
	case PM_STATE_MODE_IDLE:
	case PM_STATE_MODE_STANDBY:
		offp.power_domains |= PD_SSE700_AON_MASK;
		offp.ip_clock_gating = 0;
		offp.phy_pwr_gating = 0;
		offp.dcdc_mode = DCDC_MODE_PFM_AUTO;
		break;
	case PM_STATE_MODE_STOP:
		offp.ip_clock_gating = 0;
		offp.phy_pwr_gating = 0;
		offp.dcdc_mode = DCDC_MODE_OFF;
		break;
	}

	offp.aon_clk_src = CLK_SRC_LFXO;
#if OFF_HFRC
	offp.stby_clk_src = CLK_SRC_HFRC;
	offp.stby_clk_freq = SCALED_FREQ_RC_STDBY_0_075_MHZ;
#else
	offp.stby_clk_src = CLK_SRC_PLL;
	offp.stby_clk_freq = SCALED_FREQ_XO_HIGH_DIV_38_4_MHZ;
#endif
	offp.ewic_cfg = (SE_OFFP_EWIC_CFG | LPGPIO_EWIC_CFG | LPCMP_EWIC_CFG);
	offp.wakeup_events = (SE_OFFP_WAKEUP_EVENTS | LPGPIO_WAKEUP_EVENT | LPCMP_WAKEUP_EVENT);
	offp.vtor_address = SCB->VTOR;
	offp.vtor_address_ns = SCB->VTOR;

	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		LOG_ERR("SE: set_off_cfg failed = %d", ret);
	}

	return ret;
}

/**
 * Set the RUN profile parameters for this application.
 */
static int app_set_run_params(void)
{
	run_profile_t runp;
	int ret;

	runp.power_domains = PD_VBAT_AON_MASK | PD_SYST_MASK | PD_SSE700_AON_MASK | PD_SESS_MASK;
	runp.dcdc_voltage = 775;
	runp.dcdc_mode = DCDC_MODE_PFM_FORCED;
	runp.aon_clk_src = CLK_SRC_LFXO;

#if RUN_HFRC
	runp.run_clk_src = CLK_SRC_HFRC;
	runp.cpu_clk_freq = CLOCK_FREQUENCY_76_8_RC_MHZ;
	runp.scaled_clk_freq = SCALED_FREQ_RC_ACTIVE_76_8_MHZ;
#else /* PLL */
	runp.run_clk_src = CLK_SRC_PLL;
	runp.cpu_clk_freq = CLOCK_FREQUENCY_160MHZ;
	runp.scaled_clk_freq = SCALED_FREQ_XO_HIGH_DIV_38_4_MHZ;
#endif

	runp.phy_pwr_gating = 0;
	runp.ip_clock_gating = 0;
	runp.vdd_ioflex_3V3 = IOFLEX_LEVEL_1V8;

	runp.memory_blocks = MRAM_MASK;
	runp.memory_blocks |= SERAM_MEMORY_BLOCKS_IN_USE;
	runp.memory_blocks |= APP_RET_MEM_BLOCKS;

	if (IS_ENABLED(CONFIG_MIPI_DSI)) {
		runp.phy_pwr_gating |= MIPI_TX_DPHY_MASK | MIPI_RX_DPHY_MASK | MIPI_PLL_DPHY_MASK;
		runp.ip_clock_gating |= CDC200_MASK | MIPI_DSI_MASK | GPU_MASK;
	}

	ret = se_service_set_run_cfg(&runp);

	return ret;
}
/*
 * CRITICAL: Must run at PRE_KERNEL_1 to restore SYSTOP before peripherals initialize.
 *
 * On cold boot: SYSTOP is already ON by default, safe to call.
 * On SOFT_OFF wakeup: SYSTOP is OFF, must restore BEFORE peripherals access registers.
 */
SYS_INIT(app_set_run_params, PRE_KERNEL_1, 3);

static inline uint32_t get_wakeup_irq_status(void)
{
	uint32_t status = WAKEUP_COLD;

	status |= NVIC_GetPendingIRQ(WAKEUP_SOURCE_IRQ) ? WAKEUP_TIMER : 0;
#if LPGPIO_WAKEUP_ENABLED
	status |= NVIC_GetPendingIRQ(DT_IRQ_BY_IDX(
		LPGPIO_NODE, CONFIG_LPGPIO_WAKEUP_SOURCE, irq)) ? WAKEUP_LPGPIO : 0;
#endif
#if LPCMP_ENABLED
	status |= NVIC_GetPendingIRQ(DT_IRQ_BY_IDX(LPCMP_NODE, 0, irq)) ? WAKEUP_LPCMP : 0;
#endif
	return status;
}

/**
 * PM Notifier callback for power state entry
 */
static void pm_notify_state_entry(enum pm_state const state)
{
	/* TODO: enable when this is needed */
	/*
	const struct pm_state_info *next_state = pm_state_next_get(0);
	uint8_t substate_id = next_state ? next_state->substate_id : 0;
	*/

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
	case PM_STATE_SOFT_OFF:
		break;
	default:
		__ASSERT(false, "Entering unknown power state %d", state);
		LOG_ERR("Entering unknown power state %d", state);
		break;
	}
}

/**
 * PM Notifier callback called BEFORE devices are resumed
 *
 * This restores SE run configuration when resuming from S2RAM states.
 * Note: For SOFT_OFF, the system resets completely and app_set_run_params()
 * runs during normal PRE_KERNEL_1 initialization, so this callback is not needed.
 */
static void pm_notify_pre_device_resume(enum pm_state const state)
{
	wakeup_status = get_wakeup_irq_status();

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM: {
		run_profile_error = app_set_run_params();
		break;
	}
	case PM_STATE_SOFT_OFF: {
		/* No action needed - SOFT_OFF causes reset, not resume */
		break;
	}
	default: {
		__ASSERT(false, "Pre-resume for unknown power state %d", state);
		LOG_ERR("Pre-resume for unknown power state %d", state);
		break;
	}
	}
}

/**
 * PM Notifier structure
 */
static struct pm_notifier app_pm_notifier = {
	.state_entry = pm_notify_state_entry,
	.pre_device_resume = pm_notify_pre_device_resume,
};

static void app_disable_sleep(void)
{
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
}

static void app_allow_sleep(void)
{
	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
}

/*
 * This function will be invoked in the PRE_KERNEL_1 phase of the init
 * routine to prevent sleep during startup.
 */
static int app_pre_kernel_init(void)
{
	/* Register PM notifier callbacks */
	pm_notifier_register(&app_pm_notifier);

	app_disable_sleep();

	return 0;
}
SYS_INIT(app_pre_kernel_init, PRE_KERNEL_1, 39);

#if defined(CONFIG_CORTEX_M_SYSTICK_LPM_TIMER_HOOKS)

static uint32_t idle_timer_pre_idle;

/* Idle timer used for timer while entering the idle state */
static const struct device *idle_timer = DEVICE_DT_GET(DT_CHOSEN(zephyr_cortex_m_idle_timer));
/**
 * To simplify the driver, implement the callout to Counter API
 * as hooks that would be provided by platform drivers if
 * CONFIG_CORTEX_M_SYSTICK_LPM_TIMER_HOOKS was selected instead.
 */
void z_cms_lptim_hook_on_lpm_entry(uint64_t max_lpm_time_us)
{

	/* Store current value of the selected timer to calculate a
	 * difference in measurements after exiting the idle state.
	 */
	counter_get_value(idle_timer, &idle_timer_pre_idle);
	/**
	 * Disable the counter alarm in case it was already running.
	 */
	/* counter_cancel_channel_alarm(idle_timer, 0); */

	/* Set the alarm using timer that runs the idle.
	 * Needed rump-up/setting time, lower accurency etc. should be
	 * included in the exit-latency in the power state definition.
	 */

	struct counter_alarm_cfg cfg = {
		.callback = NULL,
		.ticks = counter_us_to_ticks(idle_timer, max_lpm_time_us) + idle_timer_pre_idle,
		.user_data = NULL,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE,
	};
	counter_set_channel_alarm(idle_timer, 0, &cfg);
}

uint64_t z_cms_lptim_hook_on_lpm_exit(void)
{
	/**
	 * Calculate how much time elapsed according to counter.
	 */
	uint32_t idle_timer_post, idle_timer_diff;

	counter_get_value(idle_timer, &idle_timer_post);

	/**
	 * Check for counter timer overflow
	 * (TODO: this doesn't work for downcounting timers!)
	 */
	if (idle_timer_pre_idle > idle_timer_post) {
		idle_timer_diff = (counter_get_top_value(idle_timer) - idle_timer_pre_idle) +
				  idle_timer_post + 1;
	} else {
		idle_timer_diff = idle_timer_post - idle_timer_pre_idle;
	}

	return (uint64_t)counter_ticks_to_us(idle_timer, idle_timer_diff);
}
#endif /* CONFIG_CORTEX_M_SYSTICK_LPM_TIMER_COUNTER */

#if LPGPIO_WAKEUP_ENABLED

#if CONFIG_LPGPIO_M55_IRQ_ENABLED
static struct gpio_callback button_cb_data;

static void button_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!k_sem_count_get(&button_wait_sem)) {
		printk("btn!\r\n");
		k_sem_give(&button_wait_sem);
	}
}
#endif

/**
 * Configure LPGPIO0 and LPGPIO1 as inputs with interrupts
 */
static int configure_lpgpio(void)
{
	int ret;
	const struct gpio_dt_spec *spec = &lpgpio_config;

	if (!spec || !spec->port) {
		/* Just ignore */
		printk("lpgpio invalid\r\n");
		return 0;
	}

	/* Configure LPGPIO0 for wakeup */
	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("LPGPIO0 device is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(spec, GPIO_INPUT | spec->dt_flags);
	if (ret != 0) {
		LOG_ERR("Failed to configure LPGPIO as input: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(spec,
#if CONFIG_LPGPIO_M55_IRQ_EDGE_RISING
					      GPIO_INT_EDGE_RISING
#elif CONFIG_LPGPIO_M55_IRQ_EDGE_FALLING
					      GPIO_INT_EDGE_FALLING
#elif CONFIG_LPGPIO_M55_IRQ_EDGE_BOTH
					      GPIO_INT_EDGE_BOTH
#else
#error "Invalid GPIO IRQ edge configuration"
#endif
	);
	if (ret != 0) {
		LOG_ERR("Failed to configure LPGPIO interrupt: %d", ret);
		return ret;
	}

#if CONFIG_LPGPIO_M55_IRQ_ENABLED
	gpio_init_callback(&button_cb_data, button_callback, BIT(spec->pin));
	ret = gpio_add_callback(spec->port, &button_cb_data);
	if (ret != 0) {
		LOG_ERR("Failed to add button callback: %d", ret);
		return ret;
	}
#endif

	LOG_DBG("LPGPIO%d configured", spec->pin);

	return 0;
}
#endif

#if LPCMP_ENABLED

#include <zephyr/drivers/comparator.h>

static void print_cmp_reqs(void)
{
	#define LPCMP_CTRL_CLKEN    (1 << 14)

	volatile uint32_t *ana_req1 = (volatile uint32_t *)ANA_VBAT_REG1;
	volatile uint32_t *ana_req2 = (volatile uint32_t *)ANA_VBAT_REG2;

	printk("req1: 0x%08X\r\n", *ana_req1);
	printk("req2: 0x%08X\r\n", *ana_req2);
}

volatile uint8_t cmp_status;
uint8_t value;

void cmp_callback(const struct device *dev, void *status)
{
	//call_back_event = 1;
	//cmp_status = *(uint8_t *)status;

	//LOG_WRN("Comparator callback! Status: %d", cmp_status);

	cmp_status++;

	/* Signal only; printing is done from the main thread. Doing UART I/O
	 * here can hang because the console may not be powered on wake.
	 */
	if (!k_sem_count_get(&button_wait_sem)) {
		k_sem_give(&button_wait_sem);
	}

	/* LPCMP IRQ is level-based and not cleared by the driver for the LP
	 * instance; mask it here so a steady input above the reference cannot
	 * storm the ISR. It is re-armed before the next sleep.
	 */
	NVIC_DisableIRQ(DT_IRQ_BY_IDX(LPCMP_NODE, 0, irq));
}

int lpcmp_configure(void)
{
	const struct device *cmp_dev = DEVICE_DT_GET(LPCMP_NODE);

	print_cmp_reqs();

	if (!device_is_ready(cmp_dev)) {
		printk("device not ready\n");
		return -1;
	}

	comparator_set_trigger_callback(cmp_dev, cmp_callback, &value);

	//comparator_set_trigger(cmp_dev, COMPARATOR_TRIGGER_BOTH_EDGES);
	comparator_set_trigger(cmp_dev, COMPARATOR_TRIGGER_RISING_EDGE);

	print_cmp_reqs();

#if 0
	while(1) {
		k_sem_reset(&button_wait_sem);
		k_sem_take(&button_wait_sem, K_FOREVER);

		printk("wait over...\r\n");
	}
#endif
	return 0;
}
#else
static inline int lpcmp_configure(void)
{
	return 0;
}
#endif


int main(void)
{
	const struct device *const wakeup_dev = DEVICE_DT_GET(WAKEUP_SOURCE);
	int ret;

#if DT_NODE_EXISTS(DEBUG_PIN_NODE)
	if (!gpio_is_ready_dt(&debug_pin)) {
		LOG_ERR("Led not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&debug_pin, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Led config failed\n");
		return 0;
	}
#endif

	if (!device_is_ready(wakeup_dev)) {
		LOG_ERR("%s: device not ready", wakeup_dev->name);
		return -1;
	}

#if RTC_WAKEUP_INTERVAL_MS
	ret = counter_start(wakeup_dev);
	if (ret) {
		LOG_ERR("Counter start failed. error: %d", ret);
		return ret;
	}
#endif

	printk("Simple sleep demo\n");

	ret = set_off_profile(PM_STATE_MODE_STOP);
	if (ret) {
		LOG_ERR("off profile set failed. error: %d", ret);
		return ret;
	}

#if LPGPIO_WAKEUP_ENABLED
	/* Configure LPGPIO pins */
	ret = configure_lpgpio();
	if (ret) {
		LOG_ERR("Failed to configure LPGPIO: %d", ret);
		return ret;
	}
#endif

	if (lpcmp_configure()) {
		LOG_ERR("Failed to configure LPCMP");
		return -1;
	}

	app_allow_sleep();

	while (1) {
		/* TODO: better error handling will be needed here! */
		if (run_profile_error) {
		    app_disable_sleep();
			LOG_ERR("app_set_run_params failed. error: %d", run_profile_error);
			return run_profile_error;
		}

#if DT_NODE_EXISTS(DEBUG_PIN_NODE)
		gpio_pin_configure_dt(&debug_pin, GPIO_OUTPUT_ACTIVE);
		gpio_pin_toggle_dt(&debug_pin);
#endif

#if CONFIG_WAIT_BEFORE_SLEEP_SECONDS
		app_disable_sleep();

		if (wakeup_status != WAKEUP_COLD) {
			printk("waiting ");
#if !RTC_WAKEUP_INTERVAL_MS
			counter_start(wakeup_dev);
#endif
			for (int i = 0; i < CONFIG_WAIT_BEFORE_SLEEP_SECONDS; i++) {
				k_sleep(K_MSEC(1000));
				if (conn_status == BT_CONN_STATE_CONNECTED) {
					/* Go back to top of while(1) to trigger proper
					 * sleep action
					 */
					continue;
				}
				printk(".");
			}
#if !RTC_WAKEUP_INTERVAL_MS
			counter_stop(wakeup_dev);
#endif
		}

		printk(" goto sleep");
		printk("\r\n");
		k_sleep(K_MSEC(100));

		app_allow_sleep();
#endif

#if !RTC_WAKEUP_INTERVAL_MS
		k_sem_reset(&button_wait_sem);
#if LPCMP_ENABLED
		/* Re-arm after resetting the semaphore: a steady high input makes
		 * the level IRQ fire immediately, and its k_sem_give must survive.
		 */
		NVIC_ClearPendingIRQ(DT_IRQ_BY_IDX(LPCMP_NODE, 0, irq));
		NVIC_EnableIRQ(DT_IRQ_BY_IDX(LPCMP_NODE, 0, irq));
		/* Bounded wait: if a level event is lost across suspend/resume,
		 * time out and re-arm instead of wedging forever.
		 */
		if (k_sem_take(&button_wait_sem, K_SECONDS(5)) != 0) {
			continue;
		}
		printk("lpcmp!\r\n");
#else
		k_sem_take(&button_wait_sem, K_FOREVER);
#endif
#else
		k_sleep(K_MSEC(RTC_WAKEUP_INTERVAL_MS));
#endif

		printk("w");
	}

	return 0;
}

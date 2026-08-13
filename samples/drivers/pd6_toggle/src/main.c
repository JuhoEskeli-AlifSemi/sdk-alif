/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * PD6 (SYSTOP) run-profile toggle - energy measurement aid.
 *
 * The RTSS-HE core stays fully awake (busy-wait, it never sleeps) and does
 * nothing but alternate the SYSTOP power domain (PD6) through SE run-profile
 * requests: PD6 ON for PHASE_MS, then PD6 OFF for PHASE_MS, forever. Point an
 * energy analyzer at VDD_SOC and the two current levels are directly visible;
 * the difference between them is the cost of keeping PD6 powered.
 *
 * There is deliberately no UART/console and no GPIO: nothing in the build may
 * touch a SYSTOP peripheral, otherwise accessing it while PD6 is OFF would
 * fault and the current delta would be contaminated by peripheral activity.
 *
 * The baseline profile (clocks, DCDC, memory) is built explicitly below from a
 * known-good configuration for this board; only the PD6 bit of power_domains is
 * changed in the loop. Everything else stays constant across both phases, so
 * the measured current delta is due solely to PD6.
 */

#include <zephyr/kernel.h>
#include <se_service.h>

/* Half-period: PD6 spends this long ON, then this long OFF. */
#define PHASE_MS 2000U

/* Inspect these with a debugger; there is no console in this build. */
static volatile int last_err;
static volatile uint32_t on_count;
static volatile uint32_t off_count;

static run_profile_t runp;

/*
 * Domains kept powered in BOTH phases. SSE700-AON is always on while the core
 * runs; DBSS keeps the debug connection alive so the counters stay readable.
 * PD6 (PD_SYST_MASK) is added/removed on top of this in the loop.
 */
#define BASE_POWER_DOMAINS (PD_SSE700_AON_MASK | PD_DBSS_MASK)

static void build_baseline_profile(void)
{
	runp.power_domains  = BASE_POWER_DOMAINS;
	runp.dcdc_voltage   = 825;
	runp.dcdc_mode      = DCDC_MODE_PWM;
	runp.aon_clk_src    = CLK_SRC_LFXO;
	runp.run_clk_src    = CLK_SRC_HFRC;
	runp.cpu_clk_freq   = CLOCK_FREQUENCY_76_8_RC_MHZ;
	runp.scaled_clk_freq = SCALED_FREQ_RC_ACTIVE_76_8_MHZ;
	runp.vdd_ioflex_3V3 = IOFLEX_LEVEL_1V8;
	runp.memory_blocks  = MRAM_MASK;
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
	runp.memory_blocks |= SRAM0_MASK;
#endif
	runp.phy_pwr_gating  = 0;
	runp.ip_clock_gating = 0;
}

int main(void)
{
	build_baseline_profile();

	while (1) {
		runp.power_domains = BASE_POWER_DOMAINS | PD_SYST_MASK; /* PD6 ON */
		last_err = se_service_set_run_cfg(&runp);
		on_count++;
		k_busy_wait(PHASE_MS * 1000U);

		runp.power_domains = BASE_POWER_DOMAINS;                /* PD6 OFF */
		last_err = se_service_set_run_cfg(&runp);
		off_count++;
		k_busy_wait(PHASE_MS * 1000U);
	}

	return 0;
}

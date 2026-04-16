/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <stdio.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <soc_common.h>
#include <se_service.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_psram_app, LOG_LEVEL_INF);

/* Number of words for the small diagnostic dump (covers exactly one 32-byte
 * HyperBus burst plus a few extra words to catch burst-boundary issues). */
#define DIAG_WORDS 24

static void barrier(uint32_t *p)
{
	ARG_UNUSED(p);
	barrier_dsync_fence_full();
	barrier_isync_fence_full();
}

/*
 * Write a pattern, read it back, dump every word for the first DIAG_WORDS
 * words and report errors beyond that.  Returns error count.
 *
 * name       : label printed in log
 * pattern_fn : pointer to function that computes expected value for word index
 * words      : how many words to test in total
 */
static uint32_t run_test(const char *name,
			 uint32_t (*pattern_fn)(uint32_t idx),
			 volatile uint32_t *ptr,
			 uint32_t words)
{
	uint32_t errors = 0;

	LOG_INF("--- %s: writing %u words ---", name, words);
	for (uint32_t i = 0; i < words; i++) {
		ptr[i] = pattern_fn(i);
	}
	barrier((uint32_t *)ptr);

	LOG_INF("--- %s: readback (first %d words, all values) ---",
		name, DIAG_WORDS);
	for (uint32_t i = 0; i < DIAG_WORDS && i < words; i++) {
		uint32_t expected = pattern_fn(i);
		uint32_t got      = ptr[i];

		LOG_INF("  [0x%04x] exp=0x%08x got=0x%08x %s",
			i * 4, expected, got,
			got == expected ? "OK" : "FAIL");
	}

	/* Count errors across the full range, report only the first few beyond
	 * what was already dumped above. */
	uint32_t reported = 0;

	for (uint32_t i = 0; i < words; i++) {
		uint32_t expected = pattern_fn(i);
		uint32_t got      = ptr[i];

		if (got != expected) {
			if (i >= DIAG_WORDS && reported < 4) {
				LOG_INF("  [0x%04x] exp=0x%08x got=0x%08x FAIL",
					i * 4, expected, got);
				reported++;
			}
			errors++;
		}
	}

	LOG_INF("--- %s: %u/%u errors ---", name, errors, words);
	return errors;
}

/* Pattern A: byte-address encoded in low byte, 0xA5 marker in high byte.
 * Shift by N bytes shows up as the low byte being off by N. */
static uint32_t pat_addr_marker(uint32_t idx)
{
	return 0xA5000000u | ((idx * sizeof(uint32_t)) & 0x00FFFFFFu);
}

/* Pattern B: all bytes identical per word — isolates byte-lane swaps.
 *   word 0 → 0x01010101, word 1 → 0x02020202, ... */
static uint32_t pat_same_bytes(uint32_t idx)
{
	uint8_t b = (uint8_t)((idx + 1) & 0xFF);

	return (uint32_t)b | ((uint32_t)b << 8) |
	       ((uint32_t)b << 16) | ((uint32_t)b << 24);
}

/* Pattern C: walking bit across all 32 bits (wraps every 32 words). */
static uint32_t pat_walking_bit(uint32_t idx)
{
	return 1u << (idx % 32);
}

/* Pattern D: alternating 0xAA / 0x55 per word — checks stuck bits. */
static uint32_t pat_aa55(uint32_t idx)
{
	return (idx & 1) ? 0x55555555u : 0xAAAAAAAAu;
}

int main(void)
{
	const struct device *psram_dev = DEVICE_DT_GET(DT_ALIAS(spi_psram));
	volatile uint32_t *const ptr =
		(volatile uint32_t *)DT_PROP_BY_IDX(
			DT_PARENT(DT_ALIAS(spi_psram)), xip_base_address, 0);
	const uint32_t ram_size = DT_PROP(DT_ALIAS(spi_psram), size);
	const uint32_t words    = ram_size / sizeof(uint32_t);
	uint32_t total_errors   = 0;

	if (!device_is_ready(psram_dev)) {
		LOG_ERR("%s: device not ready", psram_dev->name);
		return -1;
	}

	LOG_INF("PSRAM XIP diagnostic: base=0x%08x size=%u KB",
		(uint32_t)(uintptr_t)ptr, ram_size / 1024);

	/*
	 * === Probe 0: single-address write/read ===
	 * Write a canary to word[0], read it back.  Confirms the most basic
	 * XIP read path works before any aliasing analysis.
	 * Then write word[0] again with a different value to confirm writes
	 * are not sticky/cached.
	 * Then write word[1] and read both word[0] AND word[1] to check
	 * whether word[0] is aliased by a word[1] write (address stride bug).
	 */
	LOG_INF("--- Probe 0: single-address write/read ---");

	ptr[0] = 0xDEADBEEF;
	barrier((uint32_t *)ptr);
	LOG_INF("  wrote ptr[0]=0xDEADBEEF, read=0x%08x %s",
		ptr[0], ptr[0] == 0xDEADBEEFu ? "OK" : "FAIL");

	ptr[0] = 0xCAFEBABE;
	barrier((uint32_t *)ptr);
	LOG_INF("  wrote ptr[0]=0xCAFEBABE, read=0x%08x %s",
		ptr[0], ptr[0] == 0xCAFEBABEu ? "OK" : "FAIL");

	ptr[1] = 0x12345678;
	barrier((uint32_t *)ptr);
	LOG_INF("  wrote ptr[1]=0x12345678");
	LOG_INF("    ptr[0] read=0x%08x (expect 0xCAFEBABE, %s)",
		ptr[0], ptr[0] == 0xCAFEBABEu ? "unchanged OK" : "ALIASED!");
	LOG_INF("    ptr[1] read=0x%08x (expect 0x12345678, %s)",
		ptr[1], ptr[1] == 0x12345678u ? "OK" : "FAIL");

	ptr[0] = 0x00000000;
	ptr[1] = 0x00000000;
	barrier((uint32_t *)ptr);

	/* Run four targeted patterns over the first 256 words (1 KB).
	 * Each pattern is chosen to expose a different failure mode:
	 *   A — byte shift / address misalignment
	 *   B — byte-lane swap or per-lane stuck bits
	 *   C — single stuck/floating bit
	 *   D — stuck-at-0 or stuck-at-1
	 */
	const uint32_t small = 256; /* 1 KB — fast, full dump visible in log */

	total_errors += run_test("A:addr-marker", pat_addr_marker, ptr, small);
	total_errors += run_test("B:same-bytes",  pat_same_bytes,  ptr, small);
	total_errors += run_test("C:walking-bit", pat_walking_bit, ptr, small);
	total_errors += run_test("D:aa55",        pat_aa55,        ptr, small);

	LOG_INF("=== Short tests done: %u total errors ===", total_errors);

	/* If the short tests pass, do a full-size incrementing-index sweep
	 * to exercise every address. */
	if (total_errors == 0) {
		LOG_INF("Short tests passed — running full %u KB sweep",
			ram_size / 1024);

		for (uint32_t i = 0; i < words; i++) {
			ptr[i] = i;
		}
		barrier((uint32_t *)ptr);

		uint32_t full_errors = 0;
		uint32_t reported    = 0;

		for (uint32_t i = 0; i < words; i++) {
			if (ptr[i] != i) {
				if (reported < 16) {
					LOG_INF("  [0x%06x] exp=0x%08x got=0x%08x",
						i * 4, i, ptr[i]);
					reported++;
				}
				full_errors++;
			}
		}

		LOG_INF("Full sweep: %u/%u errors", full_errors, words);
		total_errors += full_errors;
	}

	LOG_INF("Done, total errors = %u", total_errors);
	return 0;
}

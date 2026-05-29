/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interactive UART menu to inspect and tweak OV5640 registers before
 * capture. Talks to the sensor directly over the same I2C bus the
 * driver uses — the driver has already powered the sensor up and
 * programmed it in ov5640_set_fmt(), so register writes here override
 * those defaults and survive into the first capture.
 */

#include "cam_config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/printk.h>

#define OV5640_NODE DT_NODELABEL(ov5640)
static const struct i2c_dt_spec ov5640_i2c = I2C_DT_SPEC_GET(OV5640_NODE);

struct reg_entry {
	const char *name;
	uint16_t addr;
	uint8_t nbytes;
	const char *hint;
};

/*
 * Registers worth exposing in the menu. Multi-byte entries are written
 * MSB-first using OV5640's natural register layout (e.g. AEC PK EXPOSURE
 * spans 0x3500..0x3502 as 4+8+8 = 20 bits).
 */
static const struct reg_entry regs[] = {
	{ "AWB mode",       0x3406, 1, "bit0: 0=auto, 1=manual" },
	{ "AWB R gain",     0x3400, 2, "12-bit, 0x400 = 1.0x" },
	{ "AWB G gain",     0x3402, 2, "12-bit, 0x400 = 1.0x" },
	{ "AWB B gain",     0x3404, 2, "12-bit, 0x400 = 1.0x" },
	{ "AEC mode",       0x3503, 1, "bit0=AGC manual, bit1=AEC manual" },
	{ "AEC PK exposure", 0x3500, 3, "20-bit, units of 1/16 line" },
	{ "AGC PK gain",    0x350A, 2, "10-bit real gain" },
	{ "JPEG Q factor",  0x4407, 1, "lower 6 bits, lower = better quality" },
};

#define NUM_REGS ARRAY_SIZE(regs)

static int reg_read(uint16_t addr, uint8_t *val)
{
	uint8_t addr_buf[2] = { addr >> 8, addr & 0xFF };

	return i2c_write_read_dt(&ov5640_i2c, addr_buf, 2, val, 1);
}

static int reg_write(uint16_t addr, uint8_t val)
{
	uint8_t buf[3] = { addr >> 8, addr & 0xFF, val };

	return i2c_write_dt(&ov5640_i2c, buf, 3);
}

static int read_entry(const struct reg_entry *e, uint32_t *out)
{
	uint32_t v = 0;

	for (uint8_t i = 0; i < e->nbytes; i++) {
		uint8_t byte = 0;
		int ret = reg_read(e->addr + i, &byte);

		if (ret) {
			return ret;
		}
		v = (v << 8) | byte;
	}
	*out = v;
	return 0;
}

static int write_entry(const struct reg_entry *e, uint32_t val)
{
	for (int i = e->nbytes - 1; i >= 0; i--) {
		int ret = reg_write(e->addr + i, val & 0xFF);

		if (ret) {
			return ret;
		}
		val >>= 8;
	}
	return 0;
}

static void show_table(void)
{
	printk("\n== OV5640 Configuration ==\n");
	for (size_t i = 0; i < NUM_REGS; i++) {
		uint32_t v = 0;
		int ret = read_entry(&regs[i], &v);
		int width = regs[i].nbytes * 2;

		if (ret) {
			printk(" [%u] %-16s @%04x = <read err %d>\n",
			       (unsigned)i, regs[i].name, regs[i].addr, ret);
			continue;
		}
		printk(" [%u] %-16s @%04x = 0x%0*x  (%s)\n",
		       (unsigned)i, regs[i].name, regs[i].addr,
		       width, v, regs[i].hint);
	}
	printk("\n r) re-read   c) continue & capture   q) skip menu\n");
}

/*
 * Parse a number written as "0x..." (hex) or plain decimal. Returns 0 on
 * success and -EINVAL when no digits were consumed or trailing garbage
 * follows the number.
 */
static int parse_number(const char *s, uint32_t *out)
{
	char *end = NULL;
	unsigned long v;
	int base = 10;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	if (*s == '\0') {
		return -EINVAL;
	}

	v = strtoul(s, &end, base);
	if (end == s) {
		return -EINVAL;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0') {
		return -EINVAL;
	}
	*out = (uint32_t)v;
	return 0;
}

static void edit_entry(size_t idx)
{
	const struct reg_entry *e = &regs[idx];
	uint32_t cur = 0;
	uint32_t new_val;
	int ret;
	char *line;
	int width = e->nbytes * 2;

	ret = read_entry(e, &cur);
	if (ret) {
		printk("Read failed: %d\n", ret);
		return;
	}

	printk("\n%s @%04x — current 0x%0*x (%s)\n",
	       e->name, e->addr, width, cur, e->hint);
	printk("New value (decimal or 0x..), empty to keep: ");
	line = console_getline();
	if (line == NULL || line[0] == '\0') {
		printk("Unchanged.\n");
		return;
	}

	if (parse_number(line, &new_val) != 0) {
		printk("Invalid number: '%s'\n", line);
		return;
	}

	if (e->nbytes < 4 && new_val >= (1U << (e->nbytes * 8))) {
		printk("Value 0x%x does not fit in %u byte(s)\n",
		       new_val, e->nbytes);
		return;
	}

	ret = write_entry(e, new_val);
	if (ret) {
		printk("Write failed: %d\n", ret);
		return;
	}

	uint32_t verify = 0;

	if (read_entry(e, &verify) == 0) {
		printk("%s set to 0x%0*x (read back 0x%0*x)\n",
		       e->name, width, new_val, width, verify);
	} else {
		printk("%s set to 0x%0*x\n", e->name, width, new_val);
	}
}

int cam_interactive_config(void)
{
	if (!device_is_ready(ov5640_i2c.bus)) {
		printk("Configuration menu: I2C bus not ready\n");
		return -ENODEV;
	}

	console_getline_init();

	printk("\nEntering OV5640 interactive configuration. "
	       "Press 'c' to capture.\n");

	while (true) {
		char *line;
		uint32_t idx;

		show_table();
		printk("> ");
		line = console_getline();
		if (line == NULL) {
			continue;
		}

		while (*line == ' ' || *line == '\t') {
			line++;
		}

		if (line[0] == '\0' || line[0] == 'c' || line[0] == 'C') {
			printk("Continuing to capture...\n");
			return 0;
		}
		if (line[0] == 'q' || line[0] == 'Q') {
			printk("Skipping menu.\n");
			return 0;
		}
		if (line[0] == 'r' || line[0] == 'R') {
			continue;
		}

		if (parse_number(line, &idx) != 0 || idx >= NUM_REGS) {
			printk("Unknown selection: '%s'\n", line);
			continue;
		}
		edit_entry((size_t)idx);
	}
}

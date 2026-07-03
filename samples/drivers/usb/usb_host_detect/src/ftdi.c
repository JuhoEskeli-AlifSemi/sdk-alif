/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTDI USB-to-serial (FT232R) vendor-specific control.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc_dwc3.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ftdi, LOG_LEVEL_INF);

#include "ftdi.h"

/* ---- FTDI vendor-specific requests ---- */
#define FTDI_SIO_RESET			0x00
#define FTDI_SIO_SET_MODEM_CTRL		0x01
#define FTDI_SIO_SET_FLOW_CTRL		0x02
#define FTDI_SIO_SET_BAUDRATE		0x03
#define FTDI_SIO_SET_DATA		0x04
#define FTDI_SIO_SET_LATENCY_TIMER	0x09

/* FTDI request type: vendor, host-to-device, interface recipient */
#define FTDI_REQTYPE_OUT	0x40

/* FTDI data format: 8 data bits, no parity, 1 stop bit */
#define FTDI_DATA_8N1		0x0008

/* FTDI modem ctrl: DTR=1, RTS=1 */
#define FTDI_DTR_ON		0x0101
#define FTDI_RTS_ON		0x0202

/*
 * FTDI baud rate divisor calculation for FT232R:
 * Base clock = 3,000,000 Hz
 * Divisor = 3000000 / baud
 * wValue = divisor & 0xFFFF
 * wIndex = (divisor >> 16) & 0xFFFF  (sub-integer bits)
 *
 * Common values:
 *   9600:   divisor = 0x4138 (312.5 -> encoded as 312 + 0.5)
 *   115200: divisor = 0x001A (26)
 */
static int ftdi_set_baudrate(const struct device *dev, uint32_t baudrate)
{
	uint16_t wValue, wIndex;

	switch (baudrate) {
	case 9600:
		wValue = 0x4138;
		wIndex = 0x0000;
		break;
	case 19200:
		wValue = 0x809C;
		wIndex = 0x0000;
		break;
	case 38400:
		wValue = 0xC04E;
		wIndex = 0x0000;
		break;
	case 57600:
		wValue = 0x0034;
		wIndex = 0x0000;
		break;
	case 115200:
		wValue = 0x001A;
		wIndex = 0x0000;
		break;
	default:
		LOG_ERR("Unsupported baud rate: %u", baudrate);
		return -EINVAL;
	}

	return uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					 FTDI_SIO_SET_BAUDRATE,
					 wValue, wIndex, 0, NULL);
}

int ftdi_init(const struct device *dev, uint32_t baudrate)
{
	int ret;

	LOG_INF("FTDI: Reset");
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_RESET, 0, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI reset failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI: Set baud rate %u", baudrate);
	ret = ftdi_set_baudrate(dev, baudrate);
	if (ret < 0) {
		LOG_ERR("FTDI set baudrate failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI: Set data format 8N1");
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_SET_DATA,
					FTDI_DATA_8N1, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI set data failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI: Disable flow control");
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_SET_FLOW_CTRL,
					0, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI set flow ctrl failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI: Set DTR+RTS");
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_SET_MODEM_CTRL,
					FTDI_DTR_ON, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI set DTR failed: %d", ret);
		return ret;
	}
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_SET_MODEM_CTRL,
					FTDI_RTS_ON, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI set RTS failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI: Set latency timer to 16ms");
	ret = uhc_dwc3_control_transfer(dev, FTDI_REQTYPE_OUT,
					FTDI_SIO_SET_LATENCY_TIMER,
					16, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("FTDI set latency failed: %d", ret);
		return ret;
	}

	LOG_INF("FTDI initialized at %u baud, 8N1", baudrate);
	return 0;
}

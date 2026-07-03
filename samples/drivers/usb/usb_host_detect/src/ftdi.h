/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTDI USB-to-serial (FT232R) vendor-specific control for the USB host
 * detection sample. FTDI adapters are not a standard USB class; they are
 * configured through vendor control requests, which is why this lives in the
 * sample rather than in a reusable host class driver.
 */

#ifndef USB_HOST_DETECT_FTDI_H_
#define USB_HOST_DETECT_FTDI_H_

#include <zephyr/device.h>

/** FTDI USB Vendor ID */
#define FTDI_VID	0x0403

/**
 * @brief Initialise an FTDI adapter for 8N1 serial at the given baud rate
 *
 * Issues the FTDI vendor control requests (reset, baud rate, data format,
 * flow control, DTR/RTS, latency timer) over EP0.
 *
 * @param dev      USB host controller device
 * @param baudrate Serial baud rate (9600/19200/38400/57600/115200)
 *
 * @retval 0 on success
 * @retval -EINVAL unsupported baud rate
 * @retval negative errno on a control transfer failure
 */
int ftdi_init(const struct device *dev, uint32_t baudrate);

#endif /* USB_HOST_DETECT_FTDI_H_ */

/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Host Device Detection Sample
 *
 * This sample initializes the DWC3 USB controller in host mode and
 * monitors for device connect/disconnect events. When a device is
 * connected it reads and prints the DWC3 GHWPARAMS, port status,
 * and SNPSID registers to verify the controller is operating in
 * host mode.
 *
 * How to test:
 *   1. Build for E8 DK HP or HE core with the overlay
 *   2. Flash and open the serial console
 *   3. Plug a USB device (e.g., FTDI adapter) into the DK USB port
 *   4. Observe connection detection logs
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_host_sample, LOG_LEVEL_INF);

/* From the UHC DWC3 driver */
int uhc_dwc3_enumerate_device(const struct device *dev,
			      struct usb_device_descriptor *desc);
int uhc_dwc3_setup_device(const struct device *dev,
			  struct usb_device_descriptor *desc,
			  uint8_t *out_bulk_in_ep,
			  uint8_t *out_bulk_out_ep);
int uhc_dwc3_bulk_out(const struct device *dev,
		      const uint8_t *data, size_t len);
int uhc_dwc3_bulk_in(const struct device *dev,
		     uint8_t *data, size_t len);

/* Generic control transfer (exposed from driver) */
int uhc_dwc3_control_transfer(const struct device *dev,
			      uint8_t bmRequestType,
			      uint8_t bRequest,
			      uint16_t wValue,
			      uint16_t wIndex,
			      uint16_t wLength,
			      void *data);

/* ---- FTDI vendor-specific requests ---- */
#define FTDI_VID			0x0403
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

static int ftdi_init(const struct device *dev, uint32_t baudrate)
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

/* Get the UHC device from devicetree - the overlay creates the zephyr_uhc0 label */
#define UHC_NODE DT_NODELABEL(zephyr_uhc0)

#if !DT_NODE_EXISTS(UHC_NODE)
#error "zephyr_uhc0 node not found. Check your overlay applies snps,dwc3-host compatible."
#endif

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);

USBH_CONTROLLER_DEFINE(sample_uhs_ctx, DEVICE_DT_GET(UHC_NODE));

/* Event callback - receives UHC events and queues them */
static int usb_host_event_cb(const struct device *dev,
			     const struct uhc_event *const event)
{
	switch (event->type) {
	case UHC_EVT_DEV_CONNECTED_FS:
		LOG_INF(">>> USB device connected (Full Speed)");
		break;
	case UHC_EVT_DEV_CONNECTED_HS:
		LOG_INF(">>> USB device connected (High Speed)");
		break;
	case UHC_EVT_DEV_REMOVED:
		LOG_INF(">>> USB device disconnected");
		break;
	case UHC_EVT_RESETED:
		LOG_INF("Bus reset completed");
		break;
	case UHC_EVT_SUSPENDED:
		LOG_INF("Bus suspended");
		break;
	case UHC_EVT_RESUMED:
		LOG_INF("Bus resumed");
		break;
	case UHC_EVT_EP_REQUEST:
		LOG_INF("EP request completed (err=%d)", event->xfer->err);
		break;
	case UHC_EVT_ERROR:
		LOG_ERR("USB host error: %d", event->status);
		break;
	default:
		LOG_WRN("Unknown UHC event: %d", event->type);
		break;
	}

	return 0;
}

/* Read and print DWC3 identification registers */
static void dump_dwc3_info(const struct device *dev)
{
	/*
	 * Access the DWC3 registers directly to print hardware
	 * configuration info. The register layout is defined in usb_dwc3_hw.h.
	 */
	volatile uint32_t *base = (volatile uint32_t *)DT_REG_ADDR(UHC_NODE);

	/* xHCI Capability Registers (offsets 0x00 - 0x1C) */
	LOG_INF("=== xHCI Capability Registers ===");
	LOG_INF("  CAPLENGTH:  0x%08x", base[0]);
	LOG_INF("  HCSPARAMS1: 0x%08x", base[1]);
	LOG_INF("  HCSPARAMS2: 0x%08x", base[2]);
	LOG_INF("  HCSPARAMS3: 0x%08x", base[3]);
	LOG_INF("  HCCPARAMS1: 0x%08x", base[4]);
	LOG_INF("  DBOFF:      0x%08x", base[5]);
	LOG_INF("  RTSOFF:     0x%08x", base[6]);
	LOG_INF("  HCCPARAMS2: 0x%08x", base[7]);

	/* Parse CAPLENGTH to find operational register base */
	uint8_t caplength = base[0] & 0xFF;
	uint16_t hci_version = (base[0] >> 16) & 0xFFFF;

	LOG_INF("  Cap Length:    %u bytes", caplength);
	LOG_INF("  HCI Version:   0x%04x", hci_version);

	/* HCSPARAMS1 fields */
	uint32_t hcsp1 = base[1];

	LOG_INF("  MaxSlots:      %u", hcsp1 & 0xFF);
	LOG_INF("  MaxIntrs:      %u", (hcsp1 >> 8) & 0x7FF);
	LOG_INF("  MaxPorts:      %u", (hcsp1 >> 24) & 0xFF);

	/* HCCPARAMS1 fields */
	uint32_t hccp1 = base[4];

	LOG_INF("  AC64:          %s", (hccp1 & BIT(0)) ? "yes" : "no");
	LOG_INF("  CSZ (64B ctx): %s", (hccp1 & BIT(2)) ? "yes" : "no");
	LOG_INF("  xECP:          0x%x", (hccp1 >> 16) & 0xFFFF);

	/* DBOFF and RTSOFF */
	uint32_t dboff = base[5];
	uint32_t rtsoff = base[6];

	LOG_INF("  DBOFF:         0x%x", dboff);
	LOG_INF("  RTSOFF:        0x%x", rtsoff);

	/* Read operational registers at base + caplength */
	volatile uint32_t *opregs = (volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + caplength);

	LOG_INF("=== xHCI Operational Registers (base + 0x%x) ===", caplength);
	LOG_INF("  USBCMD:     0x%08x", opregs[0]);
	LOG_INF("  USBSTS:     0x%08x", opregs[1]);
	LOG_INF("  PAGESIZE:   0x%08x", opregs[2]);
	LOG_INF("  DNCTRL:     0x%08x", opregs[5]);
	LOG_INF("  CRCR_LO:    0x%08x", opregs[6]);
	LOG_INF("  CRCR_HI:    0x%08x", opregs[7]);
	LOG_INF("  DCBAAP_LO:  0x%08x", opregs[12]);
	LOG_INF("  DCBAAP_HI:  0x%08x", opregs[13]);
	LOG_INF("  CONFIG:     0x%08x", opregs[14]);

	/* DWC3 Global Registers (at offset 0xC100 from base) */
	volatile uint32_t *glbregs = (volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0xC100);

	uint32_t snpsid = glbregs[8]; /* GSNPSID at 0xC120 */
	uint32_t gctl = glbregs[4];   /* GCTL at 0xC110 */
	uint32_t hwp0 = glbregs[16];  /* GHWPARAMS0 at 0xC140 */

	LOG_INF("=== DWC3 Global Registers ===");
	LOG_INF("  SNPSID:     0x%08x", snpsid);
	LOG_INF("  GCTL:       0x%08x", gctl);
	LOG_INF("  GHWPARAMS0: 0x%08x", hwp0);

	uint32_t mode = hwp0 & 0x3;

	LOG_INF("  HW Mode:    %s",
		mode == 0 ? "Device Only" :
		mode == 1 ? "Host Only" :
		mode == 2 ? "DRD (Dual-Role)" : "Unknown");

	uint32_t prtcap = (gctl >> 12) & 0x3;

	LOG_INF("  Port Cap:   %s",
		prtcap == 1 ? "HOST" :
		prtcap == 2 ? "DEVICE" :
		prtcap == 3 ? "OTG" : "Unknown");

	/* Port Status register */
	volatile uint32_t *portsc = (volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0x420);
	uint32_t ps = *portsc;

	LOG_INF("=== Port Status ===");
	LOG_INF("  PORTSC:     0x%08x", ps);
	LOG_INF("    CCS (Connected): %s", (ps & BIT(0)) ? "YES" : "NO");
	LOG_INF("    PED (Enabled):   %s", (ps & BIT(1)) ? "YES" : "NO");
	LOG_INF("    PP  (Powered):   %s", (ps & BIT(9)) ? "YES" : "NO");

	if (ps & BIT(0)) {
		uint8_t speed = (ps >> 10) & 0xF;

		LOG_INF("    Speed:           %s",
			speed == 1 ? "Full Speed" :
			speed == 2 ? "Low Speed" :
			speed == 3 ? "High Speed" :
			speed == 4 ? "Super Speed" : "Unknown");
	}
}

int main(void)
{
	int err;

	LOG_INF("USB Host Device Detection Sample");
	LOG_INF("================================");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("USB host controller device not ready");
		return -ENODEV;
	}

	LOG_INF("UHC device ready: %s", uhc_dev->name);

	/* Initialize the USB host controller */
	err = uhc_init(uhc_dev, usb_host_event_cb, &sample_uhs_ctx);
	if (err) {
		LOG_ERR("Failed to initialize UHC: %d", err);
		return err;
	}

	LOG_INF("UHC initialized successfully");

	/* Dump hardware info */
	dump_dwc3_info(uhc_dev);

	/* Enable the host controller */
	err = uhc_enable(uhc_dev);
	if (err) {
		LOG_ERR("Failed to enable UHC: %d", err);
		return err;
	}

	LOG_INF("UHC enabled - waiting for USB device connections...");
	LOG_INF("Plug a USB device (e.g., FTDI adapter) into the USB port");

	/* Wait for device connection then enumerate */
	bool enumerated = false;

	while (true) {
		k_sleep(enumerated ? K_MSEC(100) : K_SECONDS(2));

		volatile uint32_t *portsc =
			(volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0x420);
		uint32_t ps = *portsc;
		bool connected = !!(ps & BIT(0));

		if (connected && !enumerated) {
			LOG_INF("Device detected, starting full setup...");
			k_sleep(K_MSEC(500)); /* Debounce / device settle */

			struct usb_device_descriptor desc;
			uint8_t bulk_in_ep, bulk_out_ep;

			err = uhc_dwc3_setup_device(uhc_dev, &desc,
						    &bulk_in_ep, &bulk_out_ep);
			if (err) {
				LOG_ERR("Device setup failed: %d", err);
				LOG_INF("Will retry in 10 seconds...");
				k_sleep(K_SECONDS(10));
				continue;
			}

			LOG_INF("Device fully configured!");
			LOG_INF("  VID=0x%04x PID=0x%04x",
				sys_le16_to_cpu(desc.idVendor),
				sys_le16_to_cpu(desc.idProduct));
			LOG_INF("  Bulk IN=0x%02x OUT=0x%02x",
				bulk_in_ep, bulk_out_ep);

			/* Initialize FTDI if this is an FTDI device */
			if (sys_le16_to_cpu(desc.idVendor) == FTDI_VID) {
				LOG_INF("=== FTDI Device Detected ===");
				err = ftdi_init(uhc_dev, 115200);
				if (err) {
					LOG_ERR("FTDI init failed: %d", err);
					k_sleep(K_SECONDS(10));
					continue;
				}
			}

			/* Test: read modem status (FTDI bulk IN always
			 * prepends 2 status bytes)
			 */
			LOG_INF("=== Bulk IN Test ===");
			uint8_t rx_buf[64];

			int rx_len = uhc_dwc3_bulk_in(uhc_dev, rx_buf,
						      sizeof(rx_buf));
			if (rx_len > 0) {
				LOG_INF("Bulk IN received %d bytes:", rx_len);
				LOG_HEXDUMP_INF(rx_buf, rx_len, "RX data");
				if (rx_len >= 2) {
					LOG_INF("  FTDI modem status: 0x%02x 0x%02x",
						rx_buf[0], rx_buf[1]);
				}
			} else if (rx_len == 0) {
				LOG_INF("Bulk IN: zero-length (no data pending)");
			} else {
				LOG_ERR("Bulk IN failed: %d", rx_len);
			}

			/* Send test string out the FTDI TX pin */
			LOG_INF("=== Bulk OUT Test ===");
			const uint8_t test_msg[] = "Hello USB Host!\r\n";

			int tx_len = uhc_dwc3_bulk_out(uhc_dev, test_msg,
						       sizeof(test_msg) - 1);
			if (tx_len > 0) {
				LOG_INF("Bulk OUT sent %d bytes", tx_len);
			} else {
				LOG_ERR("Bulk OUT failed: %d", tx_len);
			}

			enumerated = true;
		} else if (!connected && enumerated) {
			LOG_INF("Device disconnected");
			enumerated = false;
		} else if (connected && enumerated) {
			/* Poll for incoming data from the USB device */
			uint8_t rx_buf[64];
			int rx_len = uhc_dwc3_bulk_in(uhc_dev, rx_buf,
						      sizeof(rx_buf));
			if (rx_len > 2) {
				/* FTDI prepends 2 modem status bytes;
				 * actual data starts at byte 2
				 */
				int data_len = rx_len - 2;

				LOG_INF("RX %d data bytes:", data_len);
				LOG_HEXDUMP_INF(&rx_buf[2], data_len, "data");

				/* Echo it back out */
				int tx_len = uhc_dwc3_bulk_out(uhc_dev,
							       &rx_buf[2],
							       data_len);
				if (tx_len > 0) {
					LOG_INF("Echoed %d bytes back", tx_len);
				}
			} else if (rx_len == 2) {
				/* Modem status only, no data - ignore */
			} else if (rx_len == 0) {
				LOG_WRN("Bulk IN: zero length");
			} else {
				LOG_ERR("Bulk IN failed: %d", rx_len);
				/* Don't spam on persistent errors */
				k_sleep(K_SECONDS(1));
			}
		}

		if (!enumerated) {
			LOG_INF("Port status: PORTSC=0x%08x connected=%s",
				ps, connected ? "yes" : "no");
		}
	}

	return 0;
}

/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Audio Host Sample
 *
 * Enumerates a USB audio headset (headphone + mic), configures its
 * isochronous endpoints, and streams audio.
 *
 * The playback source is selectable via TEST_MODE in audio_gen.h (embedded
 * clip, silence, 440 Hz tone, or the default live mic->speaker loopback).
 * The clip/silence/tone frame generators live in audio_gen.c.
 *
 * To replace the test clip with your own WAV file:
 *   cd alif/samples/drivers/usb/usb_audio_host
 *   python3 scripts/wav_to_header.py your_file.wav src/audio_clip.h
 *
 * Tested with generic USB headsets (VID=0x3654 PID=0x4155, FS, UAC 1.0)
 *
 * Build:
 *   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
 *     alif/samples/drivers/usb/usb_audio_host \
 *     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/byteorder.h>

#include "audio_gen.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_audio_host, LOG_LEVEL_INF);

/* From the UHC DWC3 driver */
int uhc_dwc3_enumerate_device(const struct device *dev,
			      struct usb_device_descriptor *desc);
int uhc_dwc3_control_transfer(const struct device *dev,
			      uint8_t bmRequestType,
			      uint8_t bRequest,
			      uint16_t wValue,
			      uint16_t wIndex,
			      uint16_t wLength,
			      void *data);
int uhc_dwc3_configure_isoch(const struct device *dev,
			     uint8_t isoch_out_ep,
			     uint8_t isoch_in_ep,
			     uint16_t out_mps,
			     uint16_t in_mps);
int uhc_dwc3_isoch_out(const struct device *dev,
		       const uint8_t *data, size_t len);
int uhc_dwc3_isoch_start(const struct device *dev, size_t out_frame_size,
			 size_t in_frame_size);
int uhc_dwc3_isoch_loopback(const struct device *dev);
/* Get the UHC device from devicetree */
#define UHC_NODE DT_NODELABEL(zephyr_uhc0)

#if !DT_NODE_EXISTS(UHC_NODE)
#error "zephyr_uhc0 node not found. Check overlay."
#endif

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);

USBH_CONTROLLER_DEFINE(sample_uhs_ctx, DEVICE_DT_GET(UHC_NODE));

/* Audio parameters discovered from config descriptor */
struct audio_config {
	uint8_t spk_iface;	/* Speaker streaming interface number */
	uint8_t spk_alt;	/* Speaker alt setting (with endpoint) */
	uint8_t spk_ep;		/* Speaker isoch OUT endpoint address */
	uint16_t spk_mps;	/* Speaker max packet size */
	uint8_t mic_iface;	/* Mic streaming interface number */
	uint8_t mic_alt;	/* Mic alt setting (with endpoint) */
	uint8_t mic_ep;		/* Mic isoch IN endpoint address */
	uint16_t mic_mps;	/* Mic max packet size */
	uint8_t cfg_val;	/* bConfigurationValue */
};

/* Event callback */
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
	default:
		break;
	}

	return 0;
}

/*
 * Parse the configuration descriptor to find USB Audio Class
 * streaming interfaces and their isochronous endpoints.
 */
static int parse_audio_config(const uint8_t *desc, int len,
			      struct audio_config *acfg)
{
	int offset = 0;
	uint8_t cur_iface = 0;
	uint8_t cur_alt = 0;
	uint8_t cur_class = 0;
	uint8_t cur_subclass = 0;

	memset(acfg, 0, sizeof(*acfg));

	while (offset < len) {
		uint8_t dlen = desc[offset];
		uint8_t dtype = desc[offset + 1];

		if (dlen == 0) {
			break;
		}

		if (dtype == 0x02 && (offset + 6) <= len) {
			/* Configuration descriptor */
			acfg->cfg_val = desc[offset + 5];
			LOG_INF("[%3d] CONFIG: bConfigVal=%u bNumIfaces=%u",
				offset, acfg->cfg_val, desc[offset + 4]);
		}

		if (dtype == 0x04 && (offset + 9) <= len) {
			/* Interface descriptor */
			cur_iface = desc[offset + 2];
			cur_alt = desc[offset + 3];
			cur_class = desc[offset + 5];
			cur_subclass = desc[offset + 6];

			LOG_INF("[%3d] IFACE %u alt=%u eps=%u class=0x%02x sub=0x%02x",
				offset, cur_iface, cur_alt,
				desc[offset + 4], cur_class, cur_subclass);
		}

		if (dtype == 0x05 && (offset + 7) <= len) {
			/* Endpoint descriptor */
			uint8_t ep_addr = desc[offset + 2];
			uint8_t ep_attr = desc[offset + 3];
			uint16_t ep_mps = desc[offset + 4] |
					  (desc[offset + 5] << 8);

			LOG_INF("[%3d] EP 0x%02x attr=0x%02x mps=%u interval=%u",
				offset, ep_addr, ep_attr, ep_mps,
				desc[offset + 6]);

			/* Audio Streaming isoch endpoint? */
			if (cur_class == 0x01 && cur_subclass == 0x02 &&
			    (ep_attr & 0x03) == 0x01) {
				if (!(ep_addr & 0x80)) {
					/* Isoch OUT = speaker */
					acfg->spk_iface = cur_iface;
					acfg->spk_alt = cur_alt;
					acfg->spk_ep = ep_addr;
					acfg->spk_mps = ep_mps;
					LOG_INF("  -> Speaker EP 0x%02x mps=%u (iface %u alt %u)",
						ep_addr, ep_mps,
						cur_iface, cur_alt);
				} else {
					/* Isoch IN = microphone */
					acfg->mic_iface = cur_iface;
					acfg->mic_alt = cur_alt;
					acfg->mic_ep = ep_addr;
					acfg->mic_mps = ep_mps;
					LOG_INF("  -> Mic EP 0x%02x mps=%u (iface %u alt %u)",
						ep_addr, ep_mps,
						cur_iface, cur_alt);
				}
			}
		}

		if (dtype == 0x24 && (offset + 3) <= len) {
			/* Class-specific interface descriptor */
			LOG_INF("[%3d] CS_INTERFACE subtype=%u len=%u",
				offset, desc[offset + 2], dlen);
		}

		offset += dlen;
	}

	return 0;
}

int main(void)
{
	int err;

	LOG_INF("USB Audio Host Sample");
	LOG_INF("=====================");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("USB host controller device not ready");
		return -ENODEV;
	}

	err = uhc_init(uhc_dev, usb_host_event_cb, &sample_uhs_ctx);
	if (err) {
		LOG_ERR("Failed to initialize UHC: %d", err);
		return err;
	}

	err = uhc_enable(uhc_dev);
	if (err) {
		LOG_ERR("Failed to enable UHC: %d", err);
		return err;
	}

	LOG_INF("Waiting for USB audio device...");

	bool streaming = false;

	while (true) {
		if (streaming) {
			/* Stream for a fixed window, then stop so the deferred
			 * log thread can drain the buffered diagnostics. The
			 * pump busy-waits and never yields during streaming, so
			 * logs only flush once we stop and sleep below.
			 */
			static uint32_t stream_start_ms;

			if (stream_start_ms == 0) {
				stream_start_ms = k_uptime_get_32();
			}
			if ((k_uptime_get_32() - stream_start_ms) >= 10000) {
				LOG_INF("Stream window elapsed — stopping to "
					"flush logs");
				streaming = false;
				stream_start_ms = 0;
				k_sleep(K_MSEC(500)); /* let log thread drain */
				continue;
			}

			/* Playback source is selected by TEST_MODE in
			 * audio_gen.h. Loopback (the default) is pumped by the
			 * driver; the clip/silence/tone modes generate frames
			 * via audio_gen_next().
			 */
#if TEST_MODE == AUDIO_MODE_LOOPBACK
			/* Live loopback: the driver pumps mic IN -> speaker OUT
			 * each frame, dispatching both endpoints' completions
			 * off the shared event ring. The mic's async sample
			 * count drives the speaker rate, so no shedding is needed.
			 */
			int lb_ret = uhc_dwc3_isoch_loopback(uhc_dev);

			if (lb_ret < 0) {
				LOG_ERR("Loopback failed: %d", lb_ret);
				streaming = false;
				continue;
			}

			static int lb_frame_count;
			static uint32_t lb_last_log_time;

			lb_frame_count++;
			if ((lb_frame_count % 1000) == 0) {
				uint32_t now = k_uptime_get_32();
				uint32_t elapsed = now - lb_last_log_time;

				LOG_INF("Loopback: 1000 frames in %u ms",
					elapsed);
				lb_last_log_time = now;
			}

			continue;
#else
			/* Generate one playback frame (clip / silence / tone)
			 * with rate shedding applied - see audio_gen.c.
			 */
			size_t frame_bytes;
			const uint8_t *frame_ptr = audio_gen_next(&frame_bytes);

			int ret = uhc_dwc3_isoch_out(uhc_dev, frame_ptr,
						     frame_bytes);
			if (ret < 0) {
				LOG_ERR("Isoch OUT failed: %d", ret);
				streaming = false;
				continue;
			}

			/* Log timing every 1000 frames */
			static int frame_count;
			static uint32_t last_log_time;

			frame_count++;
			if ((frame_count % 1000) == 0) {
				uint32_t now = k_uptime_get_32();
				uint32_t elapsed = now - last_log_time;

				LOG_INF("Playback: 1000 frames in %u ms "
					"(%u us/frame avg)",
					elapsed, elapsed * 1000 / 1000);
				last_log_time = now;
			}

			continue;
#endif
		}

		k_sleep(K_SECONDS(2));

		volatile uint32_t *portsc =
			(volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0x420);
		uint32_t ps = *portsc;

		if (!(ps & BIT(0))) {
			LOG_INF("No device connected, PORTSC=0x%08x", ps);
			continue;
		}

		LOG_INF("Device detected, enumerating...");
		k_sleep(K_MSEC(1000));

		/* Re-check after settle */
		ps = *portsc;
		if (!(ps & BIT(0))) {
			LOG_WRN("Device gone during settle");
			continue;
		}

		/* Step 1: Enumerate */
		struct usb_device_descriptor desc;

		err = uhc_dwc3_enumerate_device(uhc_dev, &desc);
		if (err) {
			LOG_ERR("Enumeration failed: %d", err);
			k_sleep(K_SECONDS(5));
			continue;
		}

		LOG_INF("Device: VID=0x%04x PID=0x%04x Class=%u",
			sys_le16_to_cpu(desc.idVendor),
			sys_le16_to_cpu(desc.idProduct),
			desc.bDeviceClass);

		/* Step 2: Read full config descriptor */
		uint8_t cfg_hdr[9];

		err = uhc_dwc3_control_transfer(uhc_dev, 0x80, 0x06,
						0x0200, 0, 9, cfg_hdr);
		if (err < 0) {
			LOG_ERR("Get config header failed: %d", err);
			continue;
		}

		uint16_t total_len = cfg_hdr[2] | (cfg_hdr[3] << 8);

		LOG_INF("Config descriptor: total_len=%u", total_len);

		if (total_len > 512) {
			total_len = 512;
		}

		uint8_t full_cfg[512];

		err = uhc_dwc3_control_transfer(uhc_dev, 0x80, 0x06,
						0x0200, 0, total_len,
						full_cfg);
		if (err < 0) {
			LOG_ERR("Get full config failed: %d", err);
			continue;
		}

		/* Step 3: Parse audio descriptors */
		struct audio_config acfg;

		parse_audio_config(full_cfg, err, &acfg);

		if (acfg.spk_ep == 0 && acfg.mic_ep == 0) {
			LOG_ERR("No audio streaming endpoints found");
			k_sleep(K_SECONDS(10));
			continue;
		}

		LOG_INF("Audio config:");
		LOG_INF("  Speaker: EP 0x%02x mps=%u (iface %u alt %u)",
			acfg.spk_ep, acfg.spk_mps,
			acfg.spk_iface, acfg.spk_alt);
		LOG_INF("  Mic:     EP 0x%02x mps=%u (iface %u alt %u)",
			acfg.mic_ep, acfg.mic_mps,
			acfg.mic_iface, acfg.mic_alt);

		/* Step 4: Set Configuration */
		LOG_INF("=== Set Configuration %u ===", acfg.cfg_val);
		err = uhc_dwc3_control_transfer(uhc_dev, 0x00, 0x09,
						acfg.cfg_val, 0, 0, NULL);
		if (err < 0) {
			LOG_ERR("Set Configuration failed: %d", err);
			continue;
		}

		/* Step 5: Set Interface alt setting to activate audio EPs.
		 * Audio streaming interfaces start with alt=0 (zero-bandwidth)
		 * and must be switched to alt=1 to activate the isoch endpoint.
		 */
		if (acfg.spk_ep) {
			LOG_INF("=== SET_INTERFACE iface=%u alt=%u (speaker) ===",
				acfg.spk_iface, acfg.spk_alt);
			err = uhc_dwc3_control_transfer(
				uhc_dev, 0x01, 0x0B,
				acfg.spk_alt, acfg.spk_iface, 0, NULL);
			if (err < 0) {
				LOG_ERR("SET_INTERFACE speaker failed: %d",
					err);
				continue;
			}
		}

		if (acfg.mic_ep) {
			LOG_INF("=== SET_INTERFACE iface=%u alt=%u (mic) ===",
				acfg.mic_iface, acfg.mic_alt);
			err = uhc_dwc3_control_transfer(
				uhc_dev, 0x01, 0x0B,
				acfg.mic_alt, acfg.mic_iface, 0, NULL);
			if (err < 0) {
				LOG_ERR("SET_INTERFACE mic failed: %d", err);
				continue;
			}
		}

		/* Step 6: Configure isochronous endpoints in xHCI */
		LOG_INF("=== Configure Isoch Endpoints ===");
		err = uhc_dwc3_configure_isoch(uhc_dev,
					       acfg.spk_ep, acfg.mic_ep,
					       acfg.spk_mps, acfg.mic_mps);
		if (err) {
			LOG_ERR("Configure isoch EP failed: %d", err);
			continue;
		}

		LOG_INF("Audio streaming ready!");

		/* Prime the isoch rings with silence frames */
		err = uhc_dwc3_isoch_start(uhc_dev, acfg.spk_mps,
					   acfg.mic_mps);
		if (err) {
			LOG_ERR("Isoch start failed: %d", err);
			continue;
		}

		streaming = true;
	}

	return 0;
}

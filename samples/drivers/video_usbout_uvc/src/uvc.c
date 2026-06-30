/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal bulk UVC 1.10 MJPEG webcam class (sample-local). See uvc.h.
 *
 * Topology advertised to the host:
 *
 *   IAD (Video Interface Collection)
 *   +- Interface 0: VideoControl
 *   |    VC Header -> Camera Input Terminal (ID 1) -> Output Terminal (ID 2)
 *   +- Interface 1: VideoStreaming (single alt setting, one bulk IN endpoint)
 *        VS Input Header -> MJPEG Format -> MJPEG Frame -> Color Matching
 *
 * One JPEG frame is sent as one UVC payload: a 2-byte payload header (with the
 * Frame ID toggled per frame and End-Of-Frame set) followed by the JPEG bytes,
 * split across several bulk transfers and delimited by a short packet. The
 * frame pump is completion-driven from the endpoint request callback; the
 * UDC buffer pool is small (a couple of KB), so we copy the frame out a chunk
 * at a time rather than queueing it whole.
 */

#include "uvc.h"

#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uvc, LOG_LEVEL_INF);

/* USB Video Class codes (UVC 1.1 spec). */
#define UVC_SC_VIDEOCONTROL		0x01
#define UVC_SC_VIDEOSTREAMING		0x02
#define UVC_SC_VIDEO_INTERFACE_COLLECTION 0x03
#define UVC_PC_PROTOCOL_UNDEFINED	0x00

#define UVC_CS_INTERFACE		0x24

/* VideoControl interface descriptor subtypes. */
#define UVC_VC_HEADER			0x01
#define UVC_VC_INPUT_TERMINAL		0x02
#define UVC_VC_OUTPUT_TERMINAL		0x03

/* VideoStreaming interface descriptor subtypes. */
#define UVC_VS_INPUT_HEADER		0x01
#define UVC_VS_FORMAT_MJPEG		0x06
#define UVC_VS_FRAME_MJPEG		0x07
#define UVC_VS_COLORFORMAT		0x0D

/* Terminal types. */
#define UVC_ITT_CAMERA			0x0201
#define UVC_TT_STREAMING		0x0101

/* Class-specific request codes (UVC spec 4.2). */
#define UVC_SET_CUR			0x01
#define UVC_GET_CUR			0x81
#define UVC_GET_MIN			0x82
#define UVC_GET_MAX			0x83
#define UVC_GET_RES			0x84
#define UVC_GET_LEN			0x85
#define UVC_GET_INFO			0x86
#define UVC_GET_DEF			0x87

/* VideoStreaming interface control selectors (high byte of wValue). */
#define UVC_VS_PROBE_CONTROL		0x01
#define UVC_VS_COMMIT_CONTROL		0x02

/* GET_INFO: GET + SET supported. */
#define UVC_INFO_SUPPORTS_GET_SET	0x03

/* Payload header bmHeaderInfo bits. */
#define UVC_HDR_FID			0x01
#define UVC_HDR_EOF			0x02
#define UVC_HDR_EOH			0x80

/* Default frame interval, in 100 ns units. 2,000,000 = 5 fps. */
#define UVC_FRAME_INTERVAL             2000000U
/* Rough bitrate hint for the descriptors: max frame * 8 bits * 5 fps. */
#define UVC_BITRATE                    (UVC_MAX_FRAME_SIZE * 8U * 5U)

/*
 * Bulk UVC payload size, i.e. dwMaxPayloadTransferSize. A JPEG frame is sent
 * as a sequence of payloads of this size, each carrying its own 2-byte payload
 * header; the host ends a payload when it has received this many bytes (full
 * payload) or a short packet (final, partial payload), and ends the frame on
 * the End-Of-Frame header bit. We make each payload a single bulk transfer
 * (one UDC buffer), so this must be a multiple of the bulk max packet size for
 * both speeds (512 HS, 64 FS) and small enough to fit the UDC pool. 2048
 * satisfies both.
 */
#define UVC_PAYLOAD_SIZE		2048U
#define UVC_PAYLOAD_DATA		(UVC_PAYLOAD_SIZE - 2U)

/* Probe/Commit control data (UVC 1.1 layout, 34 bytes). */
struct uvc_probe {
	uint16_t bmHint;
	uint8_t bFormatIndex;
	uint8_t bFrameIndex;
	uint32_t dwFrameInterval;
	uint16_t wKeyFrameRate;
	uint16_t wPFrameRate;
	uint16_t wCompQuality;
	uint16_t wCompWindowSize;
	uint16_t wDelay;
	uint32_t dwMaxVideoFrameSize;
	uint32_t dwMaxPayloadTransferSize;
	uint32_t dwClockFrequency;
	uint8_t bmFramingInfo;
	uint8_t bPreferedVersion;
	uint8_t bMinVersion;
	uint8_t bMaxVersion;
} __packed;

/* Class-specific descriptor types. */
struct uvc_vc_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint16_t bcdUVC;
	uint16_t wTotalLength;
	uint32_t dwClockFrequency;
	uint8_t bInCollection;
	uint8_t baInterfaceNr[1];
} __packed;

struct uvc_camera_terminal {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bTerminalID;
	uint16_t wTerminalType;
	uint8_t bAssocTerminal;
	uint8_t iTerminal;
	uint16_t wObjectiveFocalLengthMin;
	uint16_t wObjectiveFocalLengthMax;
	uint16_t wOcularFocalLength;
	uint8_t bControlSize;
	uint8_t bmControls[3];
} __packed;

struct uvc_output_terminal {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bTerminalID;
	uint16_t wTerminalType;
	uint8_t bAssocTerminal;
	uint8_t bSourceID;
	uint8_t iTerminal;
} __packed;

struct uvc_vs_input_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bNumFormats;
	uint16_t wTotalLength;
	uint8_t bEndpointAddress;
	uint8_t bmInfo;
	uint8_t bTerminalLink;
	uint8_t bStillCaptureMethod;
	uint8_t bTriggerSupport;
	uint8_t bTriggerUsage;
	uint8_t bControlSize;
	uint8_t bmaControls[1];
} __packed;

struct uvc_vs_format_mjpeg {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bFormatIndex;
	uint8_t bNumFrameDescriptors;
	uint8_t bmFlags;
	uint8_t bDefaultFrameIndex;
	uint8_t bAspectRatioX;
	uint8_t bAspectRatioY;
	uint8_t bmInterlaceFlags;
	uint8_t bCopyProtect;
} __packed;

struct uvc_vs_frame_mjpeg {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bFrameIndex;
	uint8_t bmCapabilities;
	uint16_t wWidth;
	uint16_t wHeight;
	uint32_t dwMinBitRate;
	uint32_t dwMaxBitRate;
	uint32_t dwMaxVideoFrameBufferSize;
	uint32_t dwDefaultFrameInterval;
	uint8_t bFrameIntervalType;
	uint32_t dwFrameInterval[1];
} __packed;

struct uvc_vs_color_matching {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bColorPrimaries;
	uint8_t bTransferCharacteristics;
	uint8_t bMatrixCoefficients;
} __packed;

struct uvc_desc {
	struct usb_association_descriptor iad;
	struct usb_if_descriptor if0;
	struct uvc_vc_header vc_header;
	struct uvc_camera_terminal camera;
	struct uvc_output_terminal output;
	struct usb_if_descriptor if1;
	struct uvc_vs_input_header vs_header;
	struct uvc_vs_format_mjpeg vs_format;
	struct uvc_vs_frame_mjpeg vs_frame;
	struct uvc_vs_color_matching vs_color;
	struct usb_ep_descriptor ep_fs;
	struct usb_ep_descriptor ep_hs;
	struct usb_desc_header nil_desc;
};

#define UVC_VC_TOTAL_LEN						\
	(sizeof(struct uvc_vc_header) + sizeof(struct uvc_camera_terminal) +	\
	 sizeof(struct uvc_output_terminal))

#define UVC_VS_TOTAL_LEN						\
	(sizeof(struct uvc_vs_input_header) + sizeof(struct uvc_vs_format_mjpeg) + \
	 sizeof(struct uvc_vs_frame_mjpeg) + sizeof(struct uvc_vs_color_matching))

static struct uvc_desc uvc_desc = {
	.iad = {
		.bLength = sizeof(struct usb_association_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE_ASSOC,
		.bFirstInterface = 0,
		.bInterfaceCount = 2,
		.bFunctionClass = USB_BCC_VIDEO,
		.bFunctionSubClass = UVC_SC_VIDEO_INTERFACE_COLLECTION,
		.bFunctionProtocol = UVC_PC_PROTOCOL_UNDEFINED,
		.iFunction = 0,
	},

	/* VideoControl interface (no endpoints). */
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 0,
		.bInterfaceClass = USB_BCC_VIDEO,
		.bInterfaceSubClass = UVC_SC_VIDEOCONTROL,
		.bInterfaceProtocol = UVC_PC_PROTOCOL_UNDEFINED,
		.iInterface = 0,
	},
	.vc_header = {
		.bLength = sizeof(struct uvc_vc_header),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VC_HEADER,
		.bcdUVC = sys_cpu_to_le16(0x0110),
		.wTotalLength = sys_cpu_to_le16(UVC_VC_TOTAL_LEN),
		.dwClockFrequency = sys_cpu_to_le32(30000000),
		.bInCollection = 1,
		.baInterfaceNr = { 1 },
	},
	.camera = {
		.bLength = sizeof(struct uvc_camera_terminal),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VC_INPUT_TERMINAL,
		.bTerminalID = 1,
		.wTerminalType = sys_cpu_to_le16(UVC_ITT_CAMERA),
		.bAssocTerminal = 0,
		.iTerminal = 0,
		.wObjectiveFocalLengthMin = 0,
		.wObjectiveFocalLengthMax = 0,
		.wOcularFocalLength = 0,
		.bControlSize = 3,
		.bmControls = { 0, 0, 0 },
	},
	.output = {
		.bLength = sizeof(struct uvc_output_terminal),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VC_OUTPUT_TERMINAL,
		.bTerminalID = 2,
		.wTerminalType = sys_cpu_to_le16(UVC_TT_STREAMING),
		.bAssocTerminal = 0,
		.bSourceID = 1,
		.iTerminal = 0,
	},

	/* VideoStreaming interface (single alt setting, bulk IN endpoint). */
	.if1 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 1,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_BCC_VIDEO,
		.bInterfaceSubClass = UVC_SC_VIDEOSTREAMING,
		.bInterfaceProtocol = UVC_PC_PROTOCOL_UNDEFINED,
		.iInterface = 0,
	},
	.vs_header = {
		.bLength = sizeof(struct uvc_vs_input_header),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VS_INPUT_HEADER,
		.bNumFormats = 1,
		.wTotalLength = sys_cpu_to_le16(UVC_VS_TOTAL_LEN),
		.bEndpointAddress = 0x81,
		.bmInfo = 0,
		.bTerminalLink = 2,
		.bStillCaptureMethod = 0,
		.bTriggerSupport = 0,
		.bTriggerUsage = 0,
		.bControlSize = 1,
		.bmaControls = { 0 },
	},
	.vs_format = {
		.bLength = sizeof(struct uvc_vs_format_mjpeg),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VS_FORMAT_MJPEG,
		.bFormatIndex = 1,
		.bNumFrameDescriptors = 1,
		.bmFlags = 1, /* fixed-size samples */
		.bDefaultFrameIndex = 1,
		.bAspectRatioX = 0,
		.bAspectRatioY = 0,
		.bmInterlaceFlags = 0,
		.bCopyProtect = 0,
	},
	.vs_frame = {
		.bLength = sizeof(struct uvc_vs_frame_mjpeg),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VS_FRAME_MJPEG,
		.bFrameIndex = 1,
		.bmCapabilities = 0,
		.wWidth = sys_cpu_to_le16(UVC_FRAME_WIDTH),
		.wHeight = sys_cpu_to_le16(UVC_FRAME_HEIGHT),
		.dwMinBitRate = sys_cpu_to_le32(UVC_BITRATE),
		.dwMaxBitRate = sys_cpu_to_le32(UVC_BITRATE),
		.dwMaxVideoFrameBufferSize = sys_cpu_to_le32(UVC_MAX_FRAME_SIZE),
		.dwDefaultFrameInterval = sys_cpu_to_le32(UVC_FRAME_INTERVAL),
		.bFrameIntervalType = 1,
		.dwFrameInterval = { sys_cpu_to_le32(UVC_FRAME_INTERVAL) },
	},
	.vs_color = {
		.bLength = sizeof(struct uvc_vs_color_matching),
		.bDescriptorType = UVC_CS_INTERFACE,
		.bDescriptorSubtype = UVC_VS_COLORFORMAT,
		.bColorPrimaries = 1,       /* BT.709 */
		.bTransferCharacteristics = 1,
		.bMatrixCoefficients = 4,   /* SMPTE 170M (BT.601) */
	},

	.ep_fs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64),
		.bInterval = 0,
	},
	.ep_hs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512),
		.bInterval = 0,
	},

	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *uvc_fs_desc[] = {
	(struct usb_desc_header *)&uvc_desc.iad,
	(struct usb_desc_header *)&uvc_desc.if0,
	(struct usb_desc_header *)&uvc_desc.vc_header,
	(struct usb_desc_header *)&uvc_desc.camera,
	(struct usb_desc_header *)&uvc_desc.output,
	(struct usb_desc_header *)&uvc_desc.if1,
	(struct usb_desc_header *)&uvc_desc.vs_header,
	(struct usb_desc_header *)&uvc_desc.vs_format,
	(struct usb_desc_header *)&uvc_desc.vs_frame,
	(struct usb_desc_header *)&uvc_desc.vs_color,
	(struct usb_desc_header *)&uvc_desc.ep_fs,
	(struct usb_desc_header *)&uvc_desc.nil_desc,
};

static const struct usb_desc_header *uvc_hs_desc[] = {
	(struct usb_desc_header *)&uvc_desc.iad,
	(struct usb_desc_header *)&uvc_desc.if0,
	(struct usb_desc_header *)&uvc_desc.vc_header,
	(struct usb_desc_header *)&uvc_desc.camera,
	(struct usb_desc_header *)&uvc_desc.output,
	(struct usb_desc_header *)&uvc_desc.if1,
	(struct usb_desc_header *)&uvc_desc.vs_header,
	(struct usb_desc_header *)&uvc_desc.vs_format,
	(struct usb_desc_header *)&uvc_desc.vs_frame,
	(struct usb_desc_header *)&uvc_desc.vs_color,
	(struct usb_desc_header *)&uvc_desc.ep_hs,
	(struct usb_desc_header *)&uvc_desc.nil_desc,
};

/* The fixed Probe/Commit configuration we always report. */
static const struct uvc_probe uvc_probe_fixed = {
	.bmHint = 0,
	.bFormatIndex = 1,
	.bFrameIndex = 1,
	.dwFrameInterval = sys_cpu_to_le32(UVC_FRAME_INTERVAL),
	.wKeyFrameRate = 0,
	.wPFrameRate = 0,
	.wCompQuality = 0,
	.wCompWindowSize = 0,
	.wDelay = 0,
	.dwMaxVideoFrameSize = sys_cpu_to_le32(UVC_MAX_FRAME_SIZE),
	.dwMaxPayloadTransferSize = sys_cpu_to_le32(UVC_PAYLOAD_SIZE),
	.dwClockFrequency = sys_cpu_to_le32(30000000),
	.bmFramingInfo = 0,
	.bPreferedVersion = 1,
	.bMinVersion = 1,
	.bMaxVersion = 1,
};

struct uvc_data {
	struct usbd_class_data *c_data;
	bool enabled;
	atomic_t streaming;

	/* Stream start handshake. */
	struct k_sem start_sem;

	/* Frame transmit pump (one frame at a time). */
	const uint8_t *frame;
	size_t frame_len;	/* JPEG length */
	size_t data_off;	/* JPEG bytes already enqueued */
	uint8_t fid;		/* current frame id bit */
	bool tx_active;
	int tx_result;
	struct k_sem done_sem;
};

static struct uvc_data uvc_data = {
	.start_sem = Z_SEM_INITIALIZER(uvc_data.start_sem, 0, 1),
	.done_sem = Z_SEM_INITIALIZER(uvc_data.done_sem, 0, 1),
};

static uint8_t uvc_bulk_in_ep(struct usbd_class_data *const c_data)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return uvc_desc.ep_hs.bEndpointAddress;
	}

	return uvc_desc.ep_fs.bEndpointAddress;
}

static uint16_t uvc_bulk_mps(struct usbd_class_data *const c_data)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return sys_le16_to_cpu(uvc_desc.ep_hs.wMaxPacketSize);
	}

	return sys_le16_to_cpu(uvc_desc.ep_fs.wMaxPacketSize);
}

/*
 * Enqueue the next UVC payload of the current frame on the bulk IN endpoint.
 * Each payload is one bulk transfer: a 2-byte payload header followed by up to
 * UVC_PAYLOAD_DATA JPEG bytes. The Frame ID bit is constant across a frame and
 * End-Of-Frame is set only on the last payload.
 */
static int uvc_kick_payload(struct usbd_class_data *const c_data)
{
	struct uvc_data *data = usbd_class_get_private(c_data);
	uint8_t ep = uvc_bulk_in_ep(c_data);
	uint16_t mps = uvc_bulk_mps(c_data);
	struct net_buf *buf;
	size_t remaining = data->frame_len - data->data_off;
	size_t data_len = MIN(remaining, (size_t)UVC_PAYLOAD_DATA);
	bool last = (data_len == remaining);
	size_t wire_len = data_len + 2;
	uint8_t header[2];
	int err;

	/*
	 * The host needs a terminator for each payload: a full payload
	 * (== UVC_PAYLOAD_SIZE) is delimited on the byte count, but a short,
	 * final payload must end with a USB short packet (a transfer whose
	 * length is not a multiple of the max packet size).
	 *
	 * This DWC3 controller driver ignores the ZLP request flag, so we
	 * cannot ask it to append a terminating zero-length packet. If a short
	 * final payload would otherwise land on an exact multiple of the max
	 * packet size, shave one byte so the transfer ends with a short packet.
	 * That makes this payload non-final; the shaved byte rolls into the
	 * next (tiny) payload, which then carries End-Of-Frame.
	 */
	if (last && wire_len < UVC_PAYLOAD_SIZE && (wire_len % mps) == 0) {
		data_len -= 1;
		wire_len -= 1;
		last = false;
	}

	header[0] = 2;
	header[1] = UVC_HDR_EOH | (data->fid ? UVC_HDR_FID : 0) |
		    (last ? UVC_HDR_EOF : 0);

	buf = usbd_ep_buf_alloc(c_data, ep, wire_len);
	if (buf == NULL) {
		LOG_ERR("Failed to allocate %zu byte TX buffer", wire_len);
		return -ENOMEM;
	}

	net_buf_add_mem(buf, header, sizeof(header));
	net_buf_add_mem(buf, data->frame + data->data_off, data_len);

	err = usbd_ep_enqueue(c_data, buf);
	if (err) {
		LOG_ERR("Failed to enqueue TX buffer: %d", err);
		net_buf_unref(buf);
		return err;
	}

	data->data_off += data_len;
	return 0;
}

static void uvc_finish_frame(struct uvc_data *data, int result)
{
	data->tx_active = false;
	data->tx_result = result;
	k_sem_give(&data->done_sem);
}

static int uvc_request(struct usbd_class_data *const c_data,
		       struct net_buf *buf, int err)
{
	struct uvc_data *data = usbd_class_get_private(c_data);
	struct udc_buf_info *bi = udc_get_buf_info(buf);
	uint8_t ep = uvc_bulk_in_ep(c_data);

	if (bi->ep != ep) {
		net_buf_unref(buf);
		return 0;
	}

	net_buf_unref(buf);

	if (!data->tx_active) {
		return 0;
	}

	if (err) {
		if (err != -ECONNABORTED) {
			LOG_ERR("Bulk IN transfer failed: %d", err);
		}
		uvc_finish_frame(data, err);
		return err;
	}

	if (data->data_off >= data->frame_len) {
		/* All payloads of the frame transferred. */
		uvc_finish_frame(data, 0);
		return 0;
	}

	return uvc_kick_payload(c_data);
}

static int uvc_control_to_host(struct usbd_class_data *const c_data,
			       const struct usb_setup_packet *const setup,
			       struct net_buf *const buf)
{
	uint8_t cs = (setup->wValue >> 8) & 0xFF;
	uint16_t len;

	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_INTERFACE) {
		errno = -ENOTSUP;
		return 0;
	}

	if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
		/* Optional terminal/unit controls: not supported -> STALL. */
		errno = -ENOTSUP;
		return 0;
	}

	switch (setup->bRequest) {
	case UVC_GET_CUR:
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
		/* Single fixed configuration: all return the same struct. */
		net_buf_add_mem(buf, &uvc_probe_fixed,
				MIN(sizeof(uvc_probe_fixed), setup->wLength));
		return 0;
	case UVC_GET_RES: {
		/* No adjustable fields. */
		uint8_t *p;

		len = MIN(sizeof(uvc_probe_fixed), setup->wLength);
		p = net_buf_add(buf, len);
		memset(p, 0, len);
		return 0;
	}
	case UVC_GET_LEN:
		len = sys_cpu_to_le16(sizeof(struct uvc_probe));
		net_buf_add_mem(buf, &len, MIN(sizeof(len), setup->wLength));
		return 0;
	case UVC_GET_INFO: {
		uint8_t info = UVC_INFO_SUPPORTS_GET_SET;

		net_buf_add_mem(buf, &info, MIN(sizeof(info), setup->wLength));
		return 0;
	}
	default:
		errno = -ENOTSUP;
		return 0;
	}
}

static int uvc_control_to_dev(struct usbd_class_data *const c_data,
			      const struct usb_setup_packet *const setup,
			      const struct net_buf *const buf)
{
	struct uvc_data *data = usbd_class_get_private(c_data);
	uint8_t cs = (setup->wValue >> 8) & 0xFF;

	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_INTERFACE) {
		errno = -ENOTSUP;
		return 0;
	}

	if (setup->bRequest != UVC_SET_CUR) {
		errno = -ENOTSUP;
		return 0;
	}

	/*
	 * We only advertise one configuration, so the host's PROBE selection
	 * is accepted as-is and ignored. COMMIT means "start streaming".
	 */
	if (cs == UVC_VS_COMMIT_CONTROL) {
		LOG_INF("Host committed stream, starting video");
		if (!atomic_set(&data->streaming, 1)) {
			k_sem_give(&data->start_sem);
		}
		return 0;
	}

	if (cs == UVC_VS_PROBE_CONTROL) {
		return 0;
	}

	errno = -ENOTSUP;
	return 0;
}

static void uvc_update(struct usbd_class_data *const c_data,
		       uint8_t iface, uint8_t alternate)
{
	ARG_UNUSED(c_data);
	LOG_DBG("Interface %u alternate %u", iface, alternate);
}

static void *uvc_get_desc(struct usbd_class_data *const c_data,
			  const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);

	if (speed == USBD_SPEED_HS) {
		return uvc_hs_desc;
	}

	return uvc_fs_desc;
}

static void uvc_enable(struct usbd_class_data *const c_data)
{
	struct uvc_data *data = usbd_class_get_private(c_data);

	LOG_INF("Enable %s", c_data->name);
	data->enabled = true;
}

static void uvc_disable(struct usbd_class_data *const c_data)
{
	struct uvc_data *data = usbd_class_get_private(c_data);

	LOG_INF("Disable %s", c_data->name);
	data->enabled = false;
	atomic_set(&data->streaming, 0);

	/* Unblock a frame transfer waiting on a host that has gone away. */
	if (data->tx_active) {
		uvc_finish_frame(data, -ECONNRESET);
	}
}

static int uvc_init(struct usbd_class_data *const c_data)
{
	struct uvc_data *data = usbd_class_get_private(c_data);

	data->c_data = c_data;

	/*
	 * The stack reassigns standard interface numbers and endpoint
	 * addresses but leaves class-specific descriptors alone. Patch the
	 * references they embed so they match the assigned values.
	 */
	uvc_desc.vc_header.baInterfaceNr[0] = uvc_desc.if1.bInterfaceNumber;
	uvc_desc.vs_header.bEndpointAddress = uvc_desc.ep_hs.bEndpointAddress;

	LOG_DBG("Init: VC iface %u, VS iface %u, bulk IN ep 0x%02x",
		uvc_desc.if0.bInterfaceNumber, uvc_desc.if1.bInterfaceNumber,
		uvc_desc.ep_hs.bEndpointAddress);

	return 0;
}

static struct usbd_class_api uvc_api = {
	.update = uvc_update,
	.control_to_host = uvc_control_to_host,
	.control_to_dev = uvc_control_to_dev,
	.request = uvc_request,
	.get_desc = uvc_get_desc,
	.enable = uvc_enable,
	.disable = uvc_disable,
	.init = uvc_init,
};

USBD_DEFINE_CLASS(uvc0, &uvc_api, &uvc_data, NULL);

/* Public API ------------------------------------------------------------- */

bool uvc_is_streaming(void)
{
	return uvc_data.enabled && atomic_get(&uvc_data.streaming);
}

int uvc_wait_for_stream(int timeout_ms)
{
	k_timeout_t timeout =
		(timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (uvc_is_streaming()) {
		return 0;
	}

	if (k_sem_take(&uvc_data.start_sem, timeout) != 0) {
		return -EAGAIN;
	}

	return 0;
}

int uvc_send_frame(const uint8_t *jpeg, size_t len)
{
	struct uvc_data *data = &uvc_data;
	int err;

	if (jpeg == NULL || len == 0) {
		return -EINVAL;
	}

	if (!uvc_is_streaming()) {
		return -ENODEV;
	}

	if (data->tx_active) {
		return -EBUSY;
	}

	data->frame = jpeg;
	data->frame_len = len;
	data->data_off = 0;
	data->fid ^= 1;
	data->tx_result = 0;
	data->tx_active = true;
	k_sem_reset(&data->done_sem);

	err = uvc_kick_payload(data->c_data);
	if (err) {
		data->tx_active = false;
		return err;
	}

	/* Wait until the whole frame has drained to the host (or the stream
	 * was torn down). The host pulls at its own pace, so this throttles
	 * the caller's capture rate to the host.
	 */
	k_sem_take(&data->done_sem, K_FOREVER);

	return data->tx_result;
}

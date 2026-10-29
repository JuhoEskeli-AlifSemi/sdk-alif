.. _usb_host_uvc:

USB Host UVC Capture
####################

Overview
********

This sample demonstrates USB host mode on Alif Ensemble DK boards using the
DWC3 controller's xHCI interface to capture video from a **USB Video Class**
(UVC) device. The device can stream over a **bulk** endpoint, as an Alif
board running the ``video_uvc`` sample does, or over an **isochronous** one,
as webcams do.

The sample:

1. Waits for a device on the DK USB host port and enumerates it
2. Reads the configuration descriptor and finds the VideoStreaming interface,
   its streaming endpoint, the formats and frame sizes it offers and, for an
   isochronous endpoint, the alternate settings that carry it, and logs them
3. Issues ``SET_CONFIGURATION``. A bulk IN streaming endpoint is configured
   with :c:func:`uhc_dwc3_configure_bulk_in`.
4. Negotiates a format and frame size with the UVC **Probe/Commit** controls:
   ``SET_CUR(PROBE)``, ``GET_CUR(PROBE)`` to read back the device's
   ``dwMaxVideoFrameSize`` and ``dwMaxPayloadTransferSize``, then
   ``SET_CUR(COMMIT)``. A bulk device starts streaming once the commit is
   accepted.
5. For an isochronous device, selects the alternate setting with the
   smallest endpoint that carries ``dwMaxPayloadTransferSize`` per service
   interval, configures that endpoint with
   :c:func:`uhc_dwc3_configure_isoch_in` and selects it with
   ``SET_INTERFACE``, which starts the stream
6. Keeps transfers queued: four 16 KiB bulk IN transfers with
   :c:func:`uhc_dwc3_bulk_in_submit` / :c:func:`uhc_dwc3_bulk_in_reap`, or
   32 isochronous IN transfers, one per service interval, with
   :c:func:`uhc_dwc3_isoch_in_submit` / :c:func:`uhc_dwc3_isoch_in_reap`.
   It strips the UVC payload headers and reassembles the frames.
7. Validates each frame and logs statistics once per second

Unplugging the device is detected and the sample waits for the next one.

The UVC protocol code (descriptor parsing, Probe/Commit and payload
reassembly) is in ``src/uvc.c``; ``src/main.c`` drives the device and the
stream.

Bulk payload framing
********************

A UVC stream is a sequence of *payloads*. Each starts with a header whose
first byte is its length (``bHeaderLength``) and second its flags
(``bmHeaderInfo``):

* **FID** (frame ID) toggles at each new frame
* **EOF** marks the last payload of a frame
* **ERR** reports a device-side error in this payload

On a bulk endpoint a payload ends with a short packet, or after exactly
``dwMaxPayloadTransferSize`` bytes. The sample therefore ends a payload when a
transfer completes short or the payload reaches that size. It sizes its
transfers so that a full-size payload always ends on a transfer boundary. A
frame ends at a payload with EOF, or when the FID toggles for devices that do
not set EOF.

An Alif board running the ``video_uvc`` sample sends each frame as a single
payload: a header padded to the controller's buffer alignment, then the whole
frame.

Isochronous streaming
*********************

On an isochronous endpoint each service interval carries one payload: a
header and up to the rest of what the endpoint carries per interval. On High
Speed that is one microframe (125 us), with up to three 1024-byte packets on
a high-bandwidth endpoint, so a camera that streams 640x480 YUY2 at 30 fps
sends some 3 KB every microframe.

The device does not wait for the host. An interval with no transfer queued
loses its data, so the sample keeps 32 transfers (4 ms) queued and resubmits
each as soon as it has been reaped. Frames are drawn on the display by a
thread of their own while the next frame is received into a second buffer; a
frame that completes while the previous one is still being drawn is checked
but not shown. An interval the controller missed, or a transfer that failed,
is counted as *lost* and marks the frame it belonged to as bad.

Frame validation
================

Each reassembled frame is counted as *bad* when:

* a payload of it had the ERR bit set or a malformed header, or it did not fit
  the frame buffer
* **uncompressed** formats: its size is not width x height x
  ``bBitsPerPixel`` / 8
* **MJPEG**: it does not start with a JPEG SOI marker (``FF D8``) and end with
  an EOI marker (``FF D9``, trailing zero padding allowed)

Requirements
************

* Alif Ensemble E8 DK
* A UVC device, connected to the DK USB host port, for example:

  * an Alif E1C SK running the ``samples/drivers/video_uvc`` sample (RGB565
    480x272, bulk)
  * a USB webcam (isochronous)

Building and Running
********************

The USB host overlay switches the USB controller to the
``snps,dwc3-host`` compatible so the UHC driver binds instead of the device
driver:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     alif/samples/drivers/usb/usb_host_uvc \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Flash the image, open the serial console and connect the UVC device.

Showing the video on the display
================================

Add ``display.overlay`` and ``display.conf`` to show the captured frames on
the DK's MW405 480x800 MIPI DSI panel:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     alif/samples/drivers/usb/usb_host_uvc \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay \
     -DEXTRA_DTC_OVERLAY_FILE=display.overlay -DEXTRA_CONF_FILE=display.conf

The display is brought up as follows:
the overlay sets the CDC200 panel timing for one RGB565 layer and adds the
CDC200, MIPI DSI and D-PHY blocks to the run profile, and at startup the
sample switches the DSI host to video mode, turns the panel on and enables
the CDC200. On the E4/E6/E8 DK the shared MIPI lanes are switched to the
display (``cam-disp-mux-gpios``) before the panel driver initialises.

Every frame that passes validation is drawn centred on the panel with
:c:func:`display_write`. RGB565 (``RGBP``) frames are copied as they are and
YUY2 frames are converted to RGB565; frames larger than the panel are shrunk
by an integer factor. Other formats, such as MJPEG, are captured and checked
but not shown. The frames are written to the live framebuffer, so some tearing
can be visible.

Configuration
=============

* :kconfig:option:`CONFIG_SAMPLE_UVC_FORMAT_INDEX`: ``bFormatIndex`` to
  stream, ``0`` (default) for the first format listed
* :kconfig:option:`CONFIG_SAMPLE_UVC_FRAME_INDEX`: ``bFrameIndex`` within that
  format, ``0`` (default) for the format's default frame
* :kconfig:option:`CONFIG_SAMPLE_UVC_FRAME_BUFFER_SIZE`: size of the frame
  reassembly buffer in SRAM0, 614400 bytes by default (640x480 at 16 bits per
  pixel). The device's ``dwMaxVideoFrameSize`` must fit. With the display
  there are two buffers of this size.

The format and frame indices are the ones the sample logs at connection.

.. note::

   The ``video_uvc`` sample on the E1C SK advertises the OV5640 frame sizes
   160x120, 320x240 and 480x272, but always captures and sends 480x272,
   whatever frame the host commits. Its default frame is 160x120, so with
   that device build with ``-DCONFIG_SAMPLE_UVC_FRAME_INDEX=3``. Otherwise
   its payloads overrun the negotiated ``dwMaxPayloadTransferSize`` and every
   frame is reported bad.

Debugging payload framing
=========================

Set :kconfig:option:`CONFIG_SAMPLE_UVC_TRACE_XFERS` to a number of
transfers (up to 64) to log the length and first bytes of each of the first
transfers once they have arrived, with their offset from the start of the
payload. Each isochronous transfer is a payload of its own. A healthy stream from ``video_uvc`` at 480x272 shows 15 transfers of
16384 bytes and a short one of 15392 bytes per frame, each frame starting
with the header bytes ``20 02`` or ``20 03``.

Statistics
==========

Once per second the sample logs the frame rate, throughput and average frame
size over the last second, followed by running totals: frames received, bad
frames, payloads with a malformed header, payloads with the ERR bit set,
frames that overflowed the buffer, transfers that timed out, and isochronous
transfers that lost their data.

An isochronous stream completes a transfer every interval, also when the
device has no data to send, so a timeout ends it: the device is enumerated
again if it is still attached.

Sample output
=============

E8 DK RTSS-HP with an E1C SK running ``video_uvc``, built with
``CONFIG_SAMPLE_UVC_FRAME_INDEX=3``:

.. code-block:: console

   <inf> usb_host_uvc: Device: VID=0x2fe3 PID=0x0009
   <inf> usb_host_uvc: UVC 1.50, VideoStreaming interface 1, bulk EP 0x81 mps 512
   <inf> usb_host_uvc:   Format 1: RGBP
   <inf> usb_host_uvc:     Frame 1: 160x120, default 30.0 fps
   <inf> usb_host_uvc:     Frame 2: 320x240, default 30.0 fps
   <inf> usb_host_uvc:     Frame 3: 480x272, default 30.0 fps
   <inf> usb_host_uvc: Streaming format 1 (RGBP) frame 3: 480x272, interval 33333300 ns
   <inf> usb_host_uvc:   dwMaxVideoFrameSize 261120, dwMaxPayloadTransferSize 261375
   <inf> usb_host_uvc: First frame: 261120 bytes, starts 20 00 20 00
   <inf> usb_host_uvc: 7.8 fps, 2050 KB/s, avg 261120 B/frame | frames 8 bad 0 | hdr err 0, stream err 0, overflow 0, timeouts 0, lost 0
   <inf> usb_host_uvc: 8.0 fps, 2088 KB/s, avg 261120 B/frame | frames 16 bad 0 | hdr err 0, stream err 0, overflow 0, timeouts 0, lost 0

Memory placement
****************

The USB controller's DMA cannot reach the M55 TCM, so ``uhc_dma.ld`` places
the driver's DMA buffers, the sample's bulk and isochronous transfer buffers
and its frame buffers in SRAM0. Each transfer buffer is aligned to its size so
that it never crosses a 64 KiB boundary, which an xHCI transfer descriptor
must not do.

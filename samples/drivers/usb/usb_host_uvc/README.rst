.. _usb_host_uvc:

USB Host UVC Capture
####################

Overview
********

Captures video from a **USB Video Class** (UVC) device with the DWC3
controller in xHCI host mode on Alif Ensemble DK boards. Both streaming
endpoint types are supported:

* **Bulk**, e.g. an Alif board running the ``video_uvc`` sample
* **Isochronous**, e.g. a USB webcam, including High-Speed high-bandwidth
  endpoints (up to 3 x 1024 bytes per microframe)

Optionally, the video is shown on the DK display (see
`Showing the video on the display`_).

Sequence:

#. Wait for a device and enumerate it.
#. Parse the configuration descriptor: VideoStreaming interface, streaming
   endpoint, formats, frame sizes and, for isochronous, the alternate
   settings. All are logged.
#. ``SET_CONFIGURATION``. Bulk: configure the endpoint with
   :c:func:`uhc_dwc3_configure_bulk_in`.
#. Negotiate format and frame size with UVC Probe/Commit (``SET_CUR(PROBE)``,
   ``GET_CUR(PROBE)``, ``SET_CUR(COMMIT)``). A bulk device starts streaming
   on the commit.
#. Isochronous: select the alternate setting with the smallest endpoint that
   carries ``dwMaxPayloadTransferSize`` per interval, configure it with
   :c:func:`uhc_dwc3_configure_isoch_in` and select it with
   ``SET_INTERFACE``, which starts the stream.
#. Keep transfers queued, strip the payload headers, reassemble and validate
   the frames, log statistics once per second.

Unplugging is detected; the sample then waits for the next device.

Source files:

* ``src/uvc.c``: descriptor parsing, Probe/Commit, payload reassembly
* ``src/main.c``: device handling and streaming
* ``src/display.c``: display output

Requirements
************

* Alif Ensemble E8 DK
* A UVC device on the DK USB host port, for example:

  * Alif E1C SK running ``samples/drivers/video_uvc`` (RGB565 480x272, bulk)
  * USB webcam (isochronous)

Building and Running
********************

``boards/alif_usb_host.overlay`` switches the USB controller to the
``snps,dwc3-host`` compatible, so the host driver binds:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     alif/samples/drivers/usb/usb_host_uvc \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Flash the image, open the serial console and connect the device.

Configuration
=============

.. list-table::
   :header-rows: 1

   * - Option
     - Default
     - Meaning
   * - :kconfig:option:`CONFIG_SAMPLE_UVC_FORMAT_INDEX`
     - 0
     - ``bFormatIndex`` to stream. 0: first format; with the display, any
       format the display can show.
   * - :kconfig:option:`CONFIG_SAMPLE_UVC_FRAME_INDEX`
     - 0
     - ``bFrameIndex`` within the format. 0: the format's default frame;
       with the display, the frame that fills the display best.
   * - :kconfig:option:`CONFIG_SAMPLE_UVC_FRAME_BUFFER_SIZE`
     - 614400
     - Frame reassembly buffer in SRAM0 (640x480 at 16 bpp). Must hold
       ``dwMaxVideoFrameSize``. Two buffers with the display.
   * - :kconfig:option:`CONFIG_SAMPLE_UVC_DISPLAY_ROTATE_CCW`
     - n
     - Turn frames counter-clockwise instead of clockwise on the display.
   * - :kconfig:option:`CONFIG_SAMPLE_UVC_TRACE_XFERS`
     - 0
     - Log length and first bytes of the first N transfers (up to 64).

Format and frame indices are as logged at connection.

.. note::

   The ``video_uvc`` sample on the E1C SK advertises 160x120, 320x240 and
   480x272 but always sends 480x272, and its default frame is 160x120.
   Without the display, build with ``-DCONFIG_SAMPLE_UVC_FRAME_INDEX=3``;
   otherwise its payloads exceed ``dwMaxPayloadTransferSize`` and every frame
   is reported bad. With the display, 480x272 is selected automatically.

Showing the video on the display
================================

Shows the video on the DK's MW405 480x800 MIPI DSI panel:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     alif/samples/drivers/usb/usb_host_uvc \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay \
     -DEXTRA_DTC_OVERLAY_FILE=display.overlay -DEXTRA_CONF_FILE=display.conf

``display.overlay``:

* CDC200 timing for one RGB565 layer
* D/AVE2D GPU node, SRAM1 enabled
* CDC200, MIPI DSI, GPU and D-PHY in the run profile

``display.conf``: display, AIPL and D/AVE2D enabled, frame buffers sized for
a full panel (768000 bytes).

At startup the sample switches the shared MIPI lanes to the display
(``cam-disp-mux-gpios``, E4/E6/E8 DK), sets DSI video mode, turns the panel
on and enables the CDC200.

**Frame selection.** Unless a frame index is set, the largest frame that fits
the panel without scaling, in either orientation, is streamed. A webcam's
800x448 is shown turned as 448x800. Turning is clockwise (picture top on the
right of the panel).

**Formats shown.** RGB565 (``RGBP``) and YUY2. Other formats, e.g. MJPEG, are
validated but not shown. Frames larger than the panel are shrunk by an
integer factor.

**Drawing.**

* Turned frames: YUY2 is converted to RGB565 with AIPL's Helium code into a
  buffer in SRAM1 (D/AVE2D cannot read YUY2); D/AVE2D then turns the RGB565
  frame into the framebuffer.
* D/AVE2D draws in strips 8 panel columns wide. Each panel row of a turned
  frame reads a frame column, and the texture cache holds about 8 frame
  rows; wider strips miss on every read.
* All other frames, and turned frames if D/AVE2D fails, are drawn by the
  CPU. ``-DCONFIG_AIPL=n -DCONFIG_DAVE2D=n`` forces the CPU path.

**Page flipping.** Each frame is drawn into a framebuffer that is not on
screen and put on screen in the vertical blanking with
:c:func:`cdc200_swap_fb`, so there is no tearing. The swap is not waited for
after drawing: the next frame is converted meanwhile, and the wait happens
only before the next framebuffer write.

**Threads.** Frames are drawn by a separate thread, while the next frame is
received into the second frame buffer. A frame that completes while the
previous one is still being drawn is validated but not shown.

Measured on the E8 DK, 800x448 YUY2 webcam at 30 fps:

.. list-table::
   :header-rows: 1

   * - Step
     - Time per frame
   * - YUY2 to RGB565 (Helium)
     - 19.6 ms
   * - Cache flush
     - 0.65 ms
   * - D/AVE2D rotation, 8-wide strips
     - 5.0 ms (41 ms as a single box)
   * - Waiting for the previous swap
     - 0 ms
   * - Frames on screen
     - 30 fps

Every 100 frames the sample logs the frame rate on screen, the average draw
time and, for GPU frames, its split into conversion, cache flush and
rotation.

Streaming details
*****************

Payloads
========

A UVC stream is a sequence of payloads, each starting with a header:
``bHeaderLength``, then ``bmHeaderInfo`` with

* **FID**: toggles at each new frame
* **EOF**: last payload of a frame
* **ERR**: device-side error in this payload

A frame ends at a payload with EOF, or when FID toggles (for devices that do
not set EOF).

Bulk
====

* A payload ends with a short packet or after exactly
  ``dwMaxPayloadTransferSize`` bytes.
* Transfers are sized so that a full-size payload ends on a transfer
  boundary.
* Four 16 KiB transfers are kept queued
  (:c:func:`uhc_dwc3_bulk_in_submit` / :c:func:`uhc_dwc3_bulk_in_reap`).
* ``video_uvc`` sends each frame as one payload: a header padded to the
  controller's buffer alignment, then the whole frame.

Isochronous
===========

* Each service interval carries one payload. On High Speed an interval is a
  microframe (125 us).
* The device does not wait for the host: an interval without a queued
  transfer loses its data. 32 transfers (4 ms) are kept queued
  (:c:func:`uhc_dwc3_isoch_in_submit` / :c:func:`uhc_dwc3_isoch_in_reap`)
  and each is resubmitted as soon as it is reaped.
* A missed interval or a failed transfer is counted as *lost* and marks its
  frame bad.
* A transfer completes every interval, even without data, so a timeout ends
  the stream; the device is then enumerated again if still attached.

Frame validation
================

A frame is *bad* when:

* a payload had the ERR bit set or a malformed header, data was lost, or the
  frame did not fit the frame buffer
* uncompressed formats: its size is not width x height x
  ``bBitsPerPixel`` / 8
* MJPEG: it does not start with SOI (``FF D8``) and end with EOI
  (``FF D9``, trailing zero padding allowed)

Statistics
==========

Logged once per second: frame rate, throughput and average frame size over
the last second, then running totals of frames, bad frames, malformed
headers, ERR payloads, buffer overflows, timeouts and lost isochronous
transfers.

Debugging payload framing
=========================

:kconfig:option:`CONFIG_SAMPLE_UVC_TRACE_XFERS` logs, once N transfers have
arrived, the length, first bytes and payload offset of each. Each
isochronous transfer is a payload of its own. A healthy ``video_uvc`` stream
at 480x272 shows 15 transfers of 16384 bytes and one of 15392 bytes per
frame, each frame starting with ``20 02`` or ``20 03``.

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

E8 DK RTSS-HP with a webcam, built with the display:

.. code-block:: console

   <inf> uvc_display: 30.0 fps on screen; drawing a frame takes 25195 us on average, plus 0 us waiting for the previous swap (100 of 100 by the GPU)
   <inf> uvc_display:   GPU frames: YUY2 conversion 19578 us, cache flush 649 us, D/AVE2D rotation 4965 us
   <inf> usb_host_uvc: 30.0 fps, 21504 KB/s, avg 716800 B/frame | frames 201 bad 0 | hdr err 0, stream err 0, overflow 0, timeouts 0, lost 0

Memory placement
****************

* The USB DMA cannot reach the M55 TCM: ``uhc_dma.ld`` places the driver's
  DMA buffers, the transfer buffers and the frame buffers in SRAM0.
* Each transfer buffer is aligned to its size, so it never crosses a 64 KiB
  boundary (an xHCI transfer descriptor must not).
* With the display: the second framebuffer is in SRAM0; the RGB565 staging
  buffer and the D/AVE2D heap are in SRAM1.

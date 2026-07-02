.. _video-sample:

Video Sample
############

Overview
********

This sample can be used to capture frame using Parallel-bus or CSI camera sensors, and store it to
the memory. The sample also supports ISP in the video pipeline. The functional behaviour is:

* Camera sensor will send out frames along with synchronisation signals either to CAM/LPCAM
  controller directly (in case of the parallel camera sensor eg. MT9M114), or to Camera Serial
  Interface MIPI CSI2 and then to CAM controller (in case of serial camera sensor eg. ARX3A0).
* MIPI CSI2 uses DPHY interface to receive data serially from serial camera sensor and convert it
  to parallel data since CAM/LPCAM controller has parallel interface.
* CAM/LPCAM controller will convert the captured frame with desired features and save it into the
  memory.
* In case ISP is selected (serial_camera_arx3a0_selfie.overlay), Camera controller will pass the
  frame to ISP IP. The ISP then writes the processed frame to memory.

* In case JPEG is selected (jpeg.overlay and jpeg.conf), the ISP's processed
  output frame will be considerd for compression.Then the JPEG writes
  the compressed frame to output memory.

For E7 use serial_camera_arx3a0.overlay, while for E8 use serial_camera_arx3a0_standard.overlay,
in case ISP is not needed and Standard camera is required. For selfie camera with ISP on E8, use
serial_camera_arx3a0_selfie.overlay and isp.conf as OVERLAY_CONFIG. In case selfie camera without
ISP is needed, remove isp node from serial_camera_arx3a0_selfie.overlay.


Requirements
************

The sample utilizes the CAM Controller IP from alif and a camera sensor. It may also use the
MIPI-CSI2 IP from Synopsys if the serial camera sensor application is built from the command line.
The camera sensors used in the parallel camera case is the MT9M114, while serial camera include
ARX3A0 camera sensor.

The JPEG encoder is supported only on the alif_e8 board and
can be selected during the build process only for this target.

  - Capture always uses a single video buffer (N_VID_BUFF = 1).

  - When the JPEG encoder is enabled, it allocates one additional buffer from
    the video buffer pool to hold the compressed output (this is not a second
    capture buffer). In that case the pool size
    (CONFIG_VIDEO_BUFFER_POOL_NUM_MAX) must be at least 2: the single capture
    buffer plus the JPEG output buffer. Without the JPEG encoder, a pool size
    of 1 is sufficient.

  - For a resolution of 480 × 480 with an NV12 input format,
    the minimum required buffer size (CONFIG_VIDEO_BUFFER_POOL_SZ_MAX) is 346223 bytes.

Supported Targets
*****************

* alif_e7_dk/rtss_hp
* alif_e7_dk/rtss_he
* alif_e1c_dk/rtss_he
* alif_b1_dk/rtss_he
* alif_e8_dk/rtss_hp
* alif_e8_dk/rtss_he

Tested Sensors
**************

* MT9M114 (Parallel)
* ARX3A0 (CSI)
* HM0360 (CSI)

Single-buffer Capture
*********************

The sample captures using a single video buffer (``N_VID_BUFF = 1``) and loops
to acquire ``N_FRAMES`` frames (10 by default), re-enqueuing the same buffer
after each frame. This demonstrates that continuous capture works with only one
buffer. The buffer address is printed with every frame so the reuse of the same
buffer is visible in the log. The behaviour is identical with and without the
ISP in the pipeline.

Building
********

The commands below build the MT9M114 selfie camera (CSI-2) application on the
E8 DevKit. Run them from the ``zephyr`` directory. To target the M55 HE core
instead of HP, replace ``rtss_hp`` with ``rtss_he`` in the board name.

Selfie camera with ISP:

.. code-block:: console

   west build -p always \
     -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     ../alif/samples/drivers/video/ \
     -DDTC_OVERLAY_FILE="$PWD/../alif/samples/drivers/video/boards/serial_camera_mt9m114_selfie.overlay" \
     -DOVERLAY_CONFIG="$PWD/../alif/samples/drivers/video/boards/isp.conf;$PWD/../alif/samples/drivers/video/boards/serial_camera_mt9m114.conf"


.. note::

   * ``serial_camera_mt9m114_selfie.overlay`` routes the camera through the ISP
     and therefore requires both ``isp.conf`` and ``serial_camera_mt9m114.conf``
     as ``OVERLAY_CONFIG``.
   * ``serial_camera_mt9m114_selfie_noisp.overlay`` is a copy of the selfie
     overlay with the ISP removed; the CAM controller streams directly to
     memory. It only needs ``serial_camera_mt9m114.conf`` (do not add
     ``isp.conf``).

Displaying an ISP frame with ffmpeg/ffplay:

With the ISP, each captured frame is stored as ``PRGB`` (RGB888 planar) at
480x480. In memory the three planes are laid out as ``[R plane][G plane]
[B plane]``, each plane being 480 x 480 = 230400 bytes (691200 bytes total).

First dump one frame from the debugger using the command printed in the log,
e.g.:

.. code-block:: console

   dump binary memory "/tmp/capture_0.bin" 0x02000060 0x020a8c5f -r

``ffmpeg`` has no native planar-RGB input in ``R, G, B`` order; its ``gbrp``
format expects the planes as ``G, B, R``. Reorder the planes first, then play
or convert (plane size = 230400 bytes):

.. code-block:: console

   cd /tmp
   dd if=capture_0.bin bs=230400 skip=1 count=1 status=none of=p_g.bin   # G plane
   dd if=capture_0.bin bs=230400 skip=2 count=1 status=none of=p_b.bin   # B plane
   dd if=capture_0.bin bs=230400 skip=0 count=1 status=none of=p_r.bin   # R plane
   cat p_g.bin p_b.bin p_r.bin > capture_0_gbrp.bin

   # View live:
   ffplay -f rawvideo -pixel_format gbrp -video_size 480x480 capture_0_gbrp.bin

   # Or save to PNG:
   ffmpeg -f rawvideo -pixel_format gbrp -video_size 480x480 -i capture_0_gbrp.bin \
     -frames:v 1 capture_0.png

.. note::

   Feeding the raw ``capture_0.bin`` directly to ``ffplay`` as ``gbrp`` without
   reordering swaps the colour channels (the ISP plane order is ``R, G, B``, not
   ``G, B, R``). The reorder step above is required for correct colours.

Selfie camera without ISP:

.. code-block:: console

   west build -p always \
     -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     ../alif/samples/drivers/video/ \
     -DDTC_OVERLAY_FILE="$PWD/../alif/samples/drivers/video/boards/serial_camera_mt9m114_selfie_noisp.overlay" \
     -DOVERLAY_CONFIG="$PWD/../alif/samples/drivers/video/boards/serial_camera_mt9m114.conf"

Displaying a non-ISP frame with ffmpeg/ffplay:

Without the ISP, the frame is the sensor's raw Bayer data in ``Y10P`` format:
one 16-bit little-endian word per pixel with the 10-bit sample in the low bits
(value range 0-1023), arranged as a ``GRBG`` Bayer pattern. There are no planes
to reorder. For the 1288x728 capture above the buffer is 1288 x 728 x 2 =
1875328 bytes.

Grayscale view (simplest; shows the raw frame with its Bayer checkerboard).
``gray10le`` scales the 10-bit values to the display range automatically, so no
manual brightening is needed:

.. code-block:: console

   # View live:
   ffplay -f rawvideo -pixel_format gray10le -video_size 1288x728 capture_0.bin

   # Or save to PNG:
   ffmpeg -f rawvideo -pixel_format gray10le -video_size 1288x728 -i capture_0.bin \
     -frames:v 1 capture_0.png

Sample Output
*************

Video app with Jpeg encoder
---------------------------

.. code-block:: console

   [00:00:00.000,000] <inf> csi2_dw: #rx_dphy_ids: 1
   [00:00:00.664,000] <inf> jpeg_hantro_vc9000e: VeriSilicon Hantro VC9000E JPEG encoder initialized
   [00:00:00.664,000] <inf> video_app: - Device name: isp@49046000
   [00:00:00.664,000] <inf> video_app: Selected camera: Selfie
   [00:00:00.664,000] <inf> video_app: - Capabilities:

   [00:00:00.664,000] <inf> video_app:   Y10P width (min, max, step)[560; 560; 0] height (min, max, step)[560; 560; 0]
   [00:00:01.204,000] <inf> dphy_dw: RX-DDR clock: 400000000
   [00:00:01.204,000] <inf> video_app: - format: NV12 480x480
   [00:00:01.204,000] <inf> video_app: Width - 480, Pitch - 720, Height - 480, Buff size - 345600
   [00:00:01.204,000] <inf> video_app: JPEG: device ready: jpeg@49044000
   [00:00:01.204,000] <inf> video_app: JPEG: Encoder Capabilities:
   [00:00:01.204,000] <inf> video_app:   Format: 0x3231564e, Size: 32x32 to 16384x16384
   [00:00:01.204,000] <inf> video_app:   Format: 0x3132564e, Size: 32x32 to 16384x16384
   [00:00:01.204,000] <inf> video_app: Jpeg: Outbuf allocated at 0x02000060 with 231023 bytes

   [00:00:01.208,000] <inf> video_app: - addr - 0x20386d8, size - 345600, bytesused - 0, resolution - 480x480
   [00:00:01.213,000] <inf> video_app: capture buffer[0]: dump binary memory "/home/$USER/capture_0.bin" 0x020386d8 0x0208ccd7 -r

   [00:00:01.213,000] <inf> video_app: - addr - 0x208cce0, size - 345600, bytesused - 0, resolution - 480x480
   [00:00:01.219,000] <inf> video_app: capture buffer[1]: dump binary memory "/home/$USER/capture_1.bin" 0x0208cce0 0x020e12df -r

   [00:00:08.220,000] <inf> video_app: Capture started
   [00:00:08.288,000] <inf> video_app: Got frame 0! size: 345600; timestamp 8288 ms
   [00:00:08.288,000] <inf> video_app: FPS: 0.0
   [00:00:08.288,000] <inf> video_app: Starting JPEG encoding...
   [00:00:08.288,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:08.289,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:08.289,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_0.jpg" 0x02000060 0x02001db3

   [00:00:08.488,000] <inf> video_app: Got frame 1! size: 345600; timestamp 8488 ms
   [00:00:08.488,000] <inf> video_app: FPS: 5.000000
   [00:00:08.488,000] <inf> video_app: Starting JPEG encoding...
   [00:00:08.488,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:08.489,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:08.489,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_1.jpg" 0x02000060 0x02005362

   [00:00:08.688,000] <inf> video_app: Got frame 2! size: 345600; timestamp 8688 ms
   [00:00:08.688,000] <inf> video_app: FPS: 5.000000
   [00:00:08.688,000] <inf> video_app: Starting JPEG encoding...
   [00:00:08.688,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:08.689,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:08.689,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_2.jpg" 0x02000060 0x020054ce

   [00:00:08.888,000] <inf> video_app: Got frame 3! size: 345600; timestamp 8888 ms
   [00:00:08.888,000] <inf> video_app: FPS: 5.000000
   [00:00:08.888,000] <inf> video_app: Starting JPEG encoding...
   [00:00:08.888,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:08.889,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:08.889,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_3.jpg" 0x02000060 0x02005575

   [00:00:09.088,000] <inf> video_app: Got frame 4! size: 345600; timestamp 9088 ms
   [00:00:09.088,000] <inf> video_app: FPS: 5.000000
   [00:00:09.088,000] <inf> video_app: Starting JPEG encoding...
   [00:00:09.088,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:09.089,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:09.089,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_4.jpg" 0x02000060 0x020054db

   [00:00:09.288,000] <inf> video_app: Got frame 5! size: 345600; timestamp 9288 ms
   [00:00:09.288,000] <inf> video_app: FPS: 5.000000
   [00:00:09.288,000] <inf> video_app: Starting JPEG encoding...
   [00:00:09.288,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:09.289,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:09.289,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_5.jpg" 0x02000060 0x02005450

   [00:00:09.488,000] <inf> video_app: Got frame 6! size: 345600; timestamp 9488 ms
   [00:00:09.488,000] <inf> video_app: FPS: 5.000000
   [00:00:09.488,000] <inf> video_app: Starting JPEG encoding...
   [00:00:09.488,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:09.489,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:09.489,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_6.jpg" 0x02000060 0x020053ee

   [00:00:09.688,000] <inf> video_app: Got frame 7! size: 345600; timestamp 9688 ms
   [00:00:09.688,000] <inf> video_app: FPS: 5.000000
   [00:00:09.688,000] <inf> video_app: Starting JPEG encoding...
   [00:00:09.688,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:09.689,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:09.689,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_7.jpg" 0x02000060 0x0200550b

   [00:00:09.888,000] <inf> video_app: Got frame 8! size: 345600; timestamp 9888 ms
   [00:00:09.888,000] <inf> video_app: FPS: 5.000000
   [00:00:09.888,000] <inf> video_app: Starting JPEG encoding...
   [00:00:09.888,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:09.889,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:09.889,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_8.jpg" 0x02000060 0x020055aa

   [00:00:10.088,000] <inf> video_app: Got frame 9! size: 345600; timestamp 10088 ms
   [00:00:10.088,000] <inf> video_app: FPS: 5.000000
   [00:00:10.088,000] <inf> video_app: Starting JPEG encoding...
   [00:00:10.088,000] <inf> video_app: Jpeg: Format set: 480x480, format: NV12
   [00:00:10.089,000] <inf> video_app: === JPEG Encoding Success===
   [00:00:10.089,000] <inf> video_app: Jpeg: Capture Image: dump memory "/home/$USER/capture_cp_9.jpg" 0x02000060 0x02005692

   [00:00:10.089,000] <inf> video_app: Calling video flush.
   [00:00:10.089,000] <inf> video_app: Calling video stream stop.


Video app without Jpeg encoder
------------------------------

.. code-block:: console

   [00:00:00.000,000] <inf> csi2_dw: #rx_dphy_ids: 1
   [00:00:00.662,000] <inf> video_app: - Device name: cam@49030000
   [00:00:00.662,000] <inf> video_app: - Capabilities:

   [00:00:00.662,000] <inf> video_app:   Y10P width (min, max, step)[560; 560; 0] height (min, max, step)[560; 560; 0]
   [00:00:01.202,000] <inf> dphy_dw: RX-DDR clock: 400000000
   [00:00:01.203,000] <inf> video_app: - format: Y10P 560x560
   [00:00:01.203,000] <inf> video_app: Width - 560, Pitch - 1120, Height - 560, Buff size - 627200
   [00:00:01.203,000] <inf> video_app: - addr - 0x2000060, size - 627200, bytesused - 0
   [00:00:01.231,000] <inf> video_app: capture buffer[0]: dump binary memory "/home/$USER/capture_0.bin" 0x02000060 0x0209925f -r

   [00:00:01.231,000] <inf> video_app: - addr - 0x2099268, size - 627200, bytesused - 0
   [00:00:01.258,000] <inf> video_app: capture buffer[1]: dump binary memory "/home/$USER/capture_1.bin" 0x02099268 0x02132467 -r

   [00:00:08.260,000] <inf> video_app: Capture started
   [00:00:08.328,000] <inf> video_app: Got frame 0! size: 627200; timestamp 8328 ms
   [00:00:08.328,000] <inf> video_app: FPS: 0.0
   [00:00:08.528,000] <inf> video_app: Got frame 1! size: 627200; timestamp 8528 ms
   [00:00:08.528,000] <inf> video_app: FPS: 5.000000
   [00:00:08.728,000] <inf> video_app: Got frame 2! size: 627200; timestamp 8728 ms
   [00:00:08.728,000] <inf> video_app: FPS: 5.000000
   [00:00:08.928,000] <inf> video_app: Got frame 3! size: 627200; timestamp 8928 ms
   [00:00:08.928,000] <inf> video_app: FPS: 5.000000
   [00:00:09.128,000] <inf> video_app: Got frame 4! size: 627200; timestamp 9128 ms
   [00:00:09.128,000] <inf> video_app: FPS: 5.000000
   [00:00:09.328,000] <inf> video_app: Got frame 5! size: 627200; timestamp 9328 ms
   [00:00:09.328,000] <inf> video_app: FPS: 5.000000
   [00:00:09.528,000] <inf> video_app: Got frame 6! size: 627200; timestamp 9528 ms
   [00:00:09.528,000] <inf> video_app: FPS: 5.000000
   [00:00:09.728,000] <inf> video_app: Got frame 7! size: 627200; timestamp 9728 ms
   [00:00:09.728,000] <inf> video_app: FPS: 5.000000
   [00:00:09.928,000] <inf> video_app: Got frame 8! size: 627200; timestamp 9928 ms
   [00:00:09.928,000] <inf> video_app: FPS: 5.000000
   [00:00:10.128,000] <inf> video_app: Got frame 9! size: 627200; timestamp 10128 ms
   [00:00:10.128,000] <inf> video_app: FPS: 5.000000
   [00:00:10.128,000] <inf> video_app: Calling video flush.
   [00:00:10.128,000] <inf> video_app: Calling video stream stop.

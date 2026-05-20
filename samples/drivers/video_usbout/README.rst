.. Copyright (C) 2026 Alif Semiconductor
   SPDX-License-Identifier: Apache-2.0

Video USB Out Sample
####################

Overview
********

This sample captures a frame from the camera sensor and exposes it as a file
on a USB mass storage device. When connected to a host PC via USB, the
captured image appears as a file (``cap_0.bin``) on a removable drive,
eliminating the need to use a debugger to dump memory.

The sample:

1. Initializes the camera and captures a single frame
2. Mounts a FAT filesystem on a RAM disk
3. Writes the captured frame data to ``cap_0.bin``
4. Enables USB mass storage so the host can read the file

Requirements
************

* A board with camera and USB support (e.g., Alif E1C SK/DK)
* A camera sensor connected to the board

Building and Running
********************

.. code-block:: console

   west build -b alif_e1c_sk/ae1c1f4051920hh/rtss_he \
       alif/samples/drivers/video_usbout \
       -- -DDTC_OVERLAY_FILE=boards/alif_e1c_sk_ae1c1f4051920hh_rtss_he.overlay

After flashing, connect the USB cable to the host PC. A removable drive
will appear containing the captured image file.

The raw image data can be viewed using tools like ``ffplay`` or converted
using ``ffmpeg``:

.. code-block:: console

   # For RGB565 160x120 (OV5640):
   ffplay -f rawvideo -pixel_format rgb565le -video_size 160x120 cap_0.bin

   # For Bayer BGGR8 320x240 (HM0360):
   ffplay -f rawvideo -pixel_format bayer_bggr8 -video_size 320x240 cap_0.bin

RAM Disk Sizing
***************

The RAM disk is configured in the board overlay with ``sector-count``.
The default of 2048 sectors (1 MB) accommodates most image sizes. Adjust
this if your captured image is larger (e.g., higher resolution or deeper
pixel format).

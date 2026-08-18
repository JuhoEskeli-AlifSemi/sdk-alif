.. Copyright (C) 2026 Alif Semiconductor
   SPDX-License-Identifier: Apache-2.0

.. _video-usbout-sample:

Video USB Out Sample
####################

Overview
********

This sample captures JPEG frames from an **OV5640** parallel camera on the
Alif **E1C SK** board and streams them to a host PC as a standard **USB Video
Class (UVC)** MJPEG webcam. When the board is connected, it enumerates as a
camera; opening it in any UVC application (``ffplay``, ``cheese``, OBS, the
Windows Camera app, ...) pulls a continuous sequence of JPEG snapshots.

This replaces the earlier USB mass-storage design, which captured a single
``capture.jpg`` into a FAT RAM disk and required a board reset to take another
picture. With UVC the host grabs as many frames as it wants, on demand,
without resetting the board.

The flow is:

1. On boot the camera pipeline is initialized and USB is enabled. The board
   appears as an MJPEG webcam.
2. When the host opens the stream (UVC Probe/Commit), the sample begins
   capturing frames.
3. Each frame is captured from the sensor and pushed out the UVC bulk IN
   endpoint as one MJPEG payload. The host pulls frames at its own pace,
   which throttles the capture loop.

Pipeline behaviour:

* The OV5640 delivers JPEG-compressed frames over the parallel CPI bus to the
  LPCAM controller, which writes the frame straight into M55 memory.
* The CPI driver runs the JPEG path in snapshot mode (one frame per
  start/stop), so each UVC frame re-arms the pipeline. The sample scans the
  buffer for the JPEG ``EOI`` marker (skipping any EXIF thumbnail) to
  determine the actual compressed size before sending it.

Implementation notes:

* The UVC class is implemented locally in the sample (``src/uvc.c`` /
  ``src/uvc.h``) on top of the ``CONFIG_USB_DEVICE_STACK_NEXT`` raw device
  stack; there is no in-tree UVC class in this Zephyr revision. It is a
  deliberately minimal, **bulk-based** MJPEG webcam advertising one fixed
  format and frame size, so Probe/Commit always returns the same fixed
  configuration.
* Because bulk streaming has no host-visible "stop" event, stream teardown is
  driven by USB disable / disconnect.

Requirements
************

* Alif E1C SK or B1 SK board with a populated OV5640 module.
* USB cable from the board to a host PC.

Supported Targets
*****************

* ``alif_e1c_sk/ae1c1f4051920hh/rtss_he``
* ``alif_b1_sk/ab1c1f4m51820ph0/rtss_he``

Both SK boards wire the OV5640 to the LP-CAM parallel bus identically, so the
sample uses the same run profile, pinctrl, ``cam_enbuf`` GPIO and JPEG
snapshot path on either target. It is intentionally not portable to other
Alif boards or sensors.

Building and Running
********************

.. code-block:: console

   # E1C SK
   west build -b alif_e1c_sk/ae1c1f4051920hh/rtss_he alif/samples/drivers/video_usbout_uvc
   west flash

   # B1 SK
   west build -b alif_b1_sk/ab1c1f4m51820ph0/rtss_he alif/samples/drivers/video_usbout_uvc
   west flash

After flashing, connect the USB cable to the host PC. The board enumerates as
a UVC webcam. To view the stream on Linux:

.. code-block:: console

   # find the device node, e.g. /dev/video0
   v4l2-ctl --list-devices
   ffplay -f v4l2 -input_format mjpeg /dev/video0

   # grab a single still
   ffmpeg -y -f v4l2 -input_format mjpeg -video_size 2592x1944 -i /dev/video4 -frames:v 1 -c:v copy frame.jpg
   
   # streaming at the full 2592x1944 resolution (5 fps)
   ffplay -f v4l2 -input_format mjpeg -video_size 2592x1944 /dev/video4

Sample Output
*************

.. code-block:: console

  *** Booting Zephyr OS build ... ***
  [00:00:00.074,000] <inf> video_usbout: - Device name: lpcam@43003000
  [00:00:00.074,000] <inf> video_usbout: - format: JPEG 2592x1944
  [00:00:00.074,000] <inf> video_usbout: - capture buffer: 688128 bytes at 0x200c0000
  [00:00:00.078,000] <inf> video_usbout: UVC webcam enabled — waiting for host to open the stream.
  [00:00:05.123,000] <inf> uvc: Host committed stream, starting video

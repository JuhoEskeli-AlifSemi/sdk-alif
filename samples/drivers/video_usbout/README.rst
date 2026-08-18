.. Copyright (C) 2026 Alif Semiconductor
   SPDX-License-Identifier: Apache-2.0

.. _video-usbout-sample:

Video USB Out Sample
####################

Overview
********

This sample captures a single JPEG snapshot from an **OV5640** parallel
camera on the Alif **E1C SK** or **B1 SK** board and exposes it as a file
on a USB mass storage device. When the board is connected to a host PC the
captured image appears as ``capture.jpg`` on a removable drive, with no
debugger needed to read the frame.

The flow is:

1. The camera pipeline is initialized and a FAT filesystem is mounted
   on a RAM disk.
2. One frame is captured and written to ``/RAM:/capture.jpg``.
3. USB MSC is enabled and the host sees the removable drive containing
   the JPEG.

Pipeline behaviour:

* The OV5640 delivers JPEG-compressed frames over the parallel CPI bus
  to the LPCAM controller, which writes the frame straight into M55
  memory.
* The CPI driver stops capture on the second VSYNC; the sample then
  scans the buffer for the JPEG ``EOI`` marker (skipping any EXIF
  thumbnail) to determine the actual compressed size before writing
  the file.

Requirements
************

* Alif E1C SK or B1 SK board with a populated OV5640 module.
* USB cable from the board to a host PC.

Supported Targets
*****************

* ``alif_e1c_sk/ae1c1f4051920hh/rtss_he``
* ``alif_b1_sk/ab1c1f4m51820ph0/rtss_he``

E1C SK and B1 SK are the same camera hardware, so the two board overlays are
identical apart from the pinctrl binding. This sample is intentionally not
portable beyond those boards — the run profile, pinctrl, ``cam_enbuf`` GPIO
and JPEG snapshot path are all OV5640 + E1C/B1 SK specific.

Building and Running
********************

.. code-block:: console

   # E1C SK
   west build -b alif_e1c_sk/ae1c1f4051920hh/rtss_he alif/samples/drivers/video_usbout
   # B1 SK
   west build -b alif_b1_sk/ab1c1f4m51820ph0/rtss_he alif/samples/drivers/video_usbout
   west flash

After flashing, connect the USB cable to the host PC. A removable drive
appears containing ``capture.jpg``. To take a new picture, reset the
board.

Sample Output
*************

.. code-block:: console

  *** Booting Zephyr OS build ... ***
  [00:00:00.074,000] <inf> video_usbout: - Device name: lpcam@43003000
  [00:00:00.074,000] <inf> video_usbout: - OV5640 reg 0x302A = 0xb0 (process BSI, revision 0)
  [00:00:00.074,000] <inf> video_usbout: - format: JPEG 2592x1944
  [00:00:00.074,000] <inf> video_usbout: - capture buffer: 430080 bytes at 0x200a1e38
  [00:00:00.078,000] <inf> video_usbout: FAT filesystem mounted on /RAM:
  [00:00:00.189,000] <inf> video_usbout: Capture started
  [00:00:00.346,000] <inf> video_usbout: Frame captured, scanning for JPEG EOI...
  [00:00:00.356,000] <inf> video_usbout: JPEG EOI at offset 158022 (154 KB), timestamp 346 ms
  [00:00:00.382,000] <inf> video_usbout: Wrote 158022 bytes to /RAM:/capture.jpg
  [00:00:00.382,000] <inf> video_usbout: USB mass storage enabled — /RAM:/capture.jpg available on host.

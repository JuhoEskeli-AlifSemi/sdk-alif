.. _video-sample:

Video Sample
#############################################

Overview
********

This sample captures a JPEG snapshot from an **OV5640** parallel camera on
the Alif **E1C SK** board and implements a *push-to-capture* flow with
deep sleep between shots:

1. On boot the camera pipeline is initialized and one capture is taken.
2. After the capture completes the M55 enters ``PM_STATE_SUSPEND_TO_RAM``
   (STOP mode) with the LPGPIO wakeup source armed.
3. Pressing the button on **LPGPIO 0** (active-low) wakes the core, the PM
   notifier restores the SE run profile, a new frame is captured, and the
   board goes back to sleep.
4. The cycle repeats forever.

Pipeline behaviour:

* The OV5640 delivers JPEG-compressed frames over the parallel CPI bus to
  the LPCAM controller, which writes the frame straight into M55 memory.
* The CPI driver stops capture on the second VSYNC; the sample then scans
  the buffer for the JPEG ``EOI`` marker (skipping any EXIF thumbnail) to
  determine the actual compressed size.

The PM bits are adapted from ``alif/samples/simple_pm`` — the off profile
is configured for STOP mode with ``EWIC_VBAT_GPIO`` / ``WE_LPGPIO0`` as the
wakeup source, and the PM notifier re-applies the camera run profile on
each S2RAM resume.

Requirements
************

* Alif E1C SK board with a populated OV5640 module.
* Push-button wired to **LPGPIO 0 / P15_0** (active-low).
* Optional: scope probe on **P0_3** for the latency markers (boot rising
  edge, capture-start falling edge, frame-received rising edge).

Supported Target
****************

* ``alif_e1c_sk/ae1c1f4051920hh/rtss_he``

This sample is intentionally not portable to other Alif boards or sensors —
the run profile, off profile, pinctrl, ``cam_enbuf`` GPIO, and JPEG
snapshot path are all OV5640 + E1C SK specific.

Building and Running
********************

.. code-block:: console

   west build -b alif_e1c_sk/ae1c1f4051920hh/rtss_he alif/samples/drivers/video
   west flash

After flashing:

* Watch the boot log on UART2 — you should see one capture finish, then
  ``Press the button to capture another picture...``.
* The M55 is now in STOP-mode S2RAM; current draw drops accordingly.
* Press the LPGPIO 0 button: the core wakes, the run profile is restored,
  a new frame is captured, and the board goes back to sleep. Repeat.

To dump a captured JPEG over the debugger (address printed by the sample):

.. code-block:: console

   dump binary memory "$HOME/capture.bin" 0x02000060 0x0209925f -r

Sample Output
*************

.. code-block:: console

  *** Booting Zephyr OS build ... ***
  [00:00:00.662,000] <inf> video_app: - Device name: cam@49030000
  [00:00:00.662,000] <inf> video_app: - Capabilities:
  [00:00:00.662,000] <inf> video_app:   JPEG width (min, max, step)[2592; 2592; 0] height (min, max, step)[1944; 1944; 0]
  [00:00:00.700,000] <inf> video_app: - format: JPEG 2592x1944
  [00:00:00.700,000] <inf> video_app: Width - 2592, Pitch - 2592, Height - 1944, Buff size - 430080
  [00:00:00.700,000] <inf> video_app: - addr - 0x2000060, size - 430080, bytesused - 0, resolution - 2592x1944
  [00:00:00.700,000] <inf> video_app: capture buffer: dump binary memory "/home/$USER/capture.bin" 0x02000060 0x0206905f -r
  [00:00:00.701,000] <inf> video_app: Capture button configured (LPGPIO0)
  [00:00:00.950,000] <inf> video_app: Capture started
  [00:00:01.180,000] <inf> video_app: Frame captured, scanning for JPEG EOI...
  [00:00:01.181,000] <inf> video_app: JPEG EOI at offset 287402 (280 KB)
  [00:00:01.181,000] <inf> video_app: Got frame! size: 287402 bytes, timestamp: 1180 ms
  [00:00:01.181,000] <inf> video_app: Press the button to capture another picture...
  (board enters S2RAM here — press LPGPIO 0)
  [00:00:25.300,000] <inf> video_app: Button pressed, starting next capture
  [00:00:25.301,000] <inf> video_app: Capture started
  ...

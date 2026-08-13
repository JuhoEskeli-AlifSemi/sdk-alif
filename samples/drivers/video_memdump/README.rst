.. _video_memdump:

Video Memory Dump (no USB)
##########################

Overview
********

No-USB sibling of ``video_usbout_uvc``. It brings up the OV5640 over the
LP-CAM parallel interface, captures a full-resolution JPEG into a DTCM buffer,
and dumps that buffer straight over the console — there is no USB device stack.

Because USB is absent, the HE core can run the aggressive scaled-HFRC
**38.4 MHz** run profile that broke USB high-speed enumeration in
``video_usbout_uvc`` (the USB PHY needs the PLL; the LP-CAM path does not).
This sample is intended to exercise and inspect low-clock capture in isolation.

Building and Running
********************

.. code-block:: console

   west build -b alif_b1_sk_ab1c1f4m51820ph0_rtss_he \
       alif/samples/drivers/video_memdump
   west flash

Usage
*****

Open the console. Press **Enter** to capture a frame. Each capture logs the
buffer address/size and the last 48 JPEG bytes, then (unless
``CONFIG_VIDEO_MEMDUMP_HEX_CONSOLE`` is disabled) prints the whole JPEG as
framed hex::

   ==JPEG_BEGIN size=<N>==
   <hex bytes...>
   ==JPEG_END==

Reconstruct the image on the host from a captured serial log:

.. code-block:: python

   import re
   t = open('capture.log').read()
   m = re.search(r'==JPEG_BEGIN size=(\d+)==\n(.*?)\n==JPEG_END==', t, re.S)
   open('frame.jpg', 'wb').write(bytes.fromhex(re.sub(r'\s', '', m.group(2))))

Disable ``CONFIG_VIDEO_MEMDUMP_HEX_CONSOLE`` to skip the (slow, at 38.4 MHz)
full-frame hex print and instead dump the buffer from the logged address with a
debugger.

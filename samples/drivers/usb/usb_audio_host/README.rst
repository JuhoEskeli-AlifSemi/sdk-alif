.. _usb_audio_host:

USB Host Audio
##############

Overview
********

This sample demonstrates USB host mode on Alif Ensemble DK boards using the
DWC3 controller's xHCI interface to talk to a **USB Audio Class** (UAC 1.0)
device — a headset with a headphone (isochronous OUT) and a microphone
(isochronous IN).

When a device is connected the sample:

1. Enumerates the device and reads its full configuration descriptor
2. Parses the USB Audio Class streaming interfaces to find the speaker
   (isoch OUT) and microphone (isoch IN) endpoints, their max packet sizes
   and alternate settings
3. Issues ``SET_CONFIGURATION`` and ``SET_INTERFACE`` to activate the audio
   endpoints (audio streaming interfaces start at alt 0 = zero-bandwidth)
4. Configures the isochronous endpoints in the xHCI controller
5. Primes the isochronous rings and starts streaming according to the
   selected test mode (see `Test modes`_ below)

The isochronous transport itself (endpoint configuration, ring priming,
per-frame OUT/IN pumping and the live loopback) lives in the DWC3 UHC driver
(``zephyr/drivers/usb/uhc/uhc_dwc3.c``); this sample handles enumeration,
audio-descriptor parsing and per-frame sample generation.

Streaming runs for a fixed ten-second window and then stops so the deferred
logging thread can drain its buffered diagnostics — the per-frame pump
busy-waits and never yields, so logs only flush once streaming pauses.

Test modes
**********

The playback source is selected at compile time by the ``TEST_MODE`` macro in
:file:`src/audio_gen.h`. It exists to isolate the source of audio artifacts by
swapping out the sample generator while keeping the USB path identical. The
clip/silence/tone frame generators live in :file:`src/audio_gen.c`.

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - ``TEST_MODE``
     - Behaviour
   * - ``0``
     - Play the embedded audio clip (a C major arpeggio) from
       ``src/audio_clip.h`` in a loop. Verifies the speaker OUT path with real
       PCM content.
   * - ``1``
     - Pure silence (all-zero frames). Confirms the OUT path runs without
       audible content.
   * - ``2``
     - Steady 440 Hz tone generated on the fly from a sine table, with
       continuous phase and no clip loop point. Isolates loop-boundary clicks
       from rate-matching clicks.
   * - ``3``
     - Live microphone → speaker loopback (**default**). The driver pumps each
       mic IN frame straight to the speaker OUT endpoint off the shared event
       ring.

To change the mode, edit ``#define TEST_MODE`` in :file:`src/audio_gen.h` and
rebuild.

Rate adaptation (TEST_MODE 0/1/2)
=================================

The host delivers ~1008.6 USB frames per second, but the headset DAC consumes a
true 48000 samples/s. Sending a flat 48 samples per frame overflows the device
FIFO roughly every 0.4 s, producing a periodic click. The playback modes use a
fractional sample-shedding accumulator, ``SHED_MILLISAMPLES`` (in
:file:`src/audio_gen.h`), to lower the effective rate:

* ``0`` — baseline, 48.000 samples/frame (clicks about every 0.4 s)
* ``410`` — 47.590 samples/frame (confirmed click-free)

The live loopback mode (``3``) needs no shedding: the microphone's asynchronous
per-frame sample count already tracks the device clock, so it drives the speaker
rate directly.

Replacing the test clip
=======================

The embedded clip in ``src/audio_clip.h`` can be regenerated from any WAV file
(48 kHz, 16-bit stereo) with the helper script:

.. code-block:: console

   cd alif/samples/drivers/usb/usb_audio_host
   python3 scripts/wav_to_header.py your_file.wav src/audio_clip.h

Then build with ``TEST_MODE 0`` to play it.

Requirements
************

* Alif Ensemble E7 or E8 Development Kit
* A USB Audio Class 1.0 headset (headphone + microphone)
* USB cable connecting the headset to the DK's USB host port

Tested with generic USB headsets (VID ``0x3654`` PID ``0x4155``, Full Speed,
UAC 1.0).

Supported Boards
****************

* ``alif_e8_dk/ae822fa0e5597xx0/rtss_hp``
* ``alif_e8_dk/ae822fa0e5597xx0/rtss_he``
* ``alif_e7_dk/ae722f80f55d5xx/rtss_hp``
* ``alif_e7_dk/ae722f80f55d5xx/rtss_he``

Building and Running
********************

Build for the E8 DK HP core:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
     alif/samples/drivers/usb/usb_audio_host \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Build for the E8 DK HE core:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_he \
     alif/samples/drivers/usb/usb_audio_host \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Flash, connect the headset, and open the serial console.

Sample Output
*************

With a USB headset connected (default ``TEST_MODE 3`` loopback):

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   [00:00:00.000,000] <inf> usb_audio_host: USB Audio Host Sample
   [00:00:00.000,000] <inf> usb_audio_host: =====================
   [00:00:00.106,000] <inf> usb_audio_host: Waiting for USB audio device...
   [00:00:02.106,000] <inf> usb_audio_host: Device detected, enumerating...
   ... driver enumeration logs (port reset, enable slot, address device) ...
   [00:00:03.200,000] <inf> usb_audio_host: Device: VID=0x3654 PID=0x4155 Class=0
   [00:00:03.300,000] <inf> usb_audio_host: Config descriptor: total_len=253
   ... parsed interface / endpoint descriptor dump ...
   [00:00:03.400,000] <inf> usb_audio_host: Audio config:
   [00:00:03.400,000] <inf> usb_audio_host:   Speaker: EP 0x01 mps=192 (iface 1 alt 1)
   [00:00:03.400,000] <inf> usb_audio_host:   Mic:     EP 0x82 mps=96 (iface 2 alt 1)
   [00:00:03.500,000] <inf> usb_audio_host: === Configure Isoch Endpoints ===
   [00:00:03.600,000] <inf> usb_audio_host: Audio streaming ready!
   [00:00:04.600,000] <inf> usb_audio_host: Loopback: 1000 frames in 1000 ms
   ...
   [00:00:13.600,000] <inf> usb_audio_host: Stream window elapsed — stopping to flush logs

Implementation Details
**********************

The DT overlay (``boards/alif_usb_host.overlay``) switches the USB node's
compatible from ``snps,dwc3`` (device mode) to ``snps,dwc3-host`` (host mode),
enables USB PHY power via AIPM, and creates the ``zephyr_uhc0`` node label.

The UHC DWC3 driver provides the isochronous helpers this sample calls:
``uhc_dwc3_configure_isoch`` (program the xHCI isoch endpoint contexts),
``uhc_dwc3_isoch_start`` (prime the rings with silence),
``uhc_dwc3_isoch_out`` (send one speaker frame) and
``uhc_dwc3_isoch_loopback`` (pump one mic-IN → speaker-OUT frame). Enumeration
and control transfers use ``uhc_dwc3_enumerate_device`` and
``uhc_dwc3_control_transfer``.

All USB host DMA buffers are placed in SRAM0 (see ``uhc_dma.ld``) because DTCM
is not reachable by the USB DMA engine.

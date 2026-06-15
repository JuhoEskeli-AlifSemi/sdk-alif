.. _usb_host_detect:

USB Host Device Detection
#########################

Overview
********

This sample demonstrates USB host mode on Alif Ensemble DK boards using the
DWC3 controller's xHCI interface. It performs full USB device enumeration:

1. Initializes the DWC3 USB controller in host mode
2. Starts the xHCI controller (command ring, event ring, DCBAA)
3. Detects device connection via PORTSC polling
4. Performs USB port reset
5. Enables an xHCI slot
6. Addresses the device
7. Reads and prints the USB device descriptor (VID/PID/class)

Plug/unplug is supported — the sample will re-enumerate on reconnection.

Requirements
************

* Alif Ensemble E7 or E8 Development Kit
* A USB device to plug in (e.g., FTDI USB-to-serial adapter)
* USB cable connecting the device to the DK's USB host port

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
     alif/samples/drivers/usb/usb_host_detect \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Build for the E8 DK HE core:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_he \
     alif/samples/drivers/usb/usb_host_detect \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Flash and open the serial console.

Sample Output
*************

With an FTDI adapter plugged in:

.. code-block:: console

   *** Booting Zephyr OS build 81dfefea2d6a ***
   [00:00:00.000,000] <inf> usb_host_sample: USB Host Device Detection Sample
   [00:00:00.000,000] <inf> usb_host_sample: ================================
   [00:00:00.000,000] <inf> usb_host_sample: UHC device ready: usb@48200000
   [00:00:00.100,000] <inf> uhc_dwc3: DWC3 UHC initialized, SNPSID=0x5533330b
   [00:00:00.106,000] <inf> usb_host_sample: UHC enabled - waiting for USB device connections...
   [00:00:02.606,000] <inf> uhc_dwc3: === Step 1: Port Reset ===
   [00:00:02.656,000] <inf> uhc_dwc3:   CCS=1 PED=1 PR=0 PP=1 Speed=1 PLS=0
   [00:00:02.757,000] <inf> uhc_dwc3: Slot 1 enabled
   [00:00:02.758,000] <inf> uhc_dwc3: Device addressed: slot_ctx[3]=0x10000001 (addr=1)
   [00:00:03.760,000] <inf> uhc_dwc3: === Device Descriptor ===
   [00:00:03.760,000] <inf> uhc_dwc3:   bLength:         18
   [00:00:03.760,000] <inf> uhc_dwc3:   bDescriptorType: 1
   [00:00:03.760,000] <inf> uhc_dwc3:   bcdUSB:          0x0200
   [00:00:03.760,000] <inf> uhc_dwc3:   idVendor:        0x0403
   [00:00:03.760,000] <inf> uhc_dwc3:   idProduct:       0x6001
   [00:00:03.760,000] <inf> uhc_dwc3:   bNumConfigurations: 1
   [00:00:03.760,000] <inf> usb_host_sample: Enumeration successful!

Implementation Details
**********************

The DT overlay (``boards/alif_usb_host.overlay``) switches the USB node's
compatible from ``snps,dwc3`` (device mode) to ``snps,dwc3-host`` (host mode),
enables USB PHY power via AIPM, and creates the ``zephyr_uhc0`` node label.

The UHC driver (``zephyr/drivers/usb/uhc/uhc_dwc3.c``) performs:

* DWC3 core soft reset and USB2 PHY configuration
* DWC3 port capability set to HOST mode
* xHCI controller initialization with DMA buffers in SRAM0
  (required because DTCM is not accessible by the USB DMA engine)
* Data cache flush/invalidate around all DMA buffer accesses

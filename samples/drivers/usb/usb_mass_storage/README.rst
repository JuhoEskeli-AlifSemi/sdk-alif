.. _usb_mass_storage:

USB Host Mass Storage
#####################

Overview
********

This sample demonstrates USB host mode on Alif Ensemble DK boards using the
DWC3 controller's xHCI interface to talk to a **USB Mass Storage** class device
(a USB flash drive).

The USB Mass Storage protocol itself is implemented by a reusable host class
driver, ``zephyr/subsys/usb/host/usbh_msc.c`` (enabled with
:kconfig:option:`CONFIG_USBH_MSC`), which exposes :c:func:`usbh_msc_probe`,
:c:func:`usbh_msc_read` / :c:func:`usbh_msc_write` and
:c:func:`usbh_msc_remove`. This sample only drives that API and layers a
filesystem on top.

:c:func:`usbh_msc_probe` performs the SCSI discovery over the USB
**Bulk-Only Transport** (BOT) protocol:

1. Enumerates the device and configures its bulk IN/OUT endpoints
2. Locates the Mass Storage interface (class ``0x08`` / subclass ``0x06`` /
   protocol ``0x50`` — SCSI transparent, Bulk-Only Transport)
3. Issues ``GET MAX LUN`` (class request)
4. Issues ``TEST UNIT READY`` and waits for the medium to become ready
5. Issues ``INQUIRY`` (vendor / product / revision strings)
6. Issues ``READ CAPACITY(10)`` (block count and block size)
7. Registers the medium with the disk-access subsystem as disk ``USB``

The sample then:

8. Dumps LBA 0 with :c:func:`usbh_msc_read` (hex dump of the first block)
9. Mounts the FAT/vfat filesystem on the drive, looks for a specific file
   (``HELLO.TXT``) and prints its details and first lines of text — or
   creates it with example content if it does not exist

Plug/unplug is supported — the sample re-runs the probe on reconnection.

Bulk-Only Transport primer
***************************

Unlike a serial adapter, a mass storage device never sends or accepts *raw*
bulk data. Every SCSI command is framed by three phases, implemented inside the
``usbh_msc`` driver's ``scsi_transfer()``:

* **Command phase** — the host sends a 31-byte Command Block Wrapper (CBW)
  on the bulk OUT endpoint. The CBW carries the SCSI command descriptor block.
* **Data phase** (optional) — data flows IN or OUT depending on the command.
* **Status phase** — the host reads a 13-byte Command Status Wrapper (CSW)
  on the bulk IN endpoint, reporting pass/fail and the data residue.

This is why a plain bulk IN times out (the device has nothing to send until it
receives a valid CBW), and why sending arbitrary bytes out the bulk OUT
endpoint makes the device STALL (it is parsed as an invalid CBW).

FAT filesystem access
*********************

After the SCSI probe, the sample mounts the drive's FAT/vfat filesystem
using Zephyr's FatFs (ELM) integration:

* The ``usbh_msc`` driver has already registered the medium with the
  disk-access subsystem under the name ``USB``
  (:kconfig:option:`CONFIG_USBH_MSC_DISK_NAME`), matching the FatFs
  volume-string table, so the volume mounts at ``/USB:``. Each logical sector
  access is mapped to a SCSI ``READ(10)`` / ``WRITE(10)`` over BOT, one
  512-byte block per command.
* USB flash drives are almost always MBR-partitioned with a single FAT volume.
  FatFs reads the MBR and mounts the first partition automatically, so the FAT
  filesystem inside the partition is found even though LBA 0 is the partition
  table (not a FAT boot sector).
* The sample then calls :c:func:`fs_stat` on ``/USB:/HELLO.TXT``. If the file
  exists it prints the size and first few lines; otherwise it creates the file
  with example text using :c:func:`fs_open`, :c:func:`fs_write` and
  :c:func:`fs_close`, then reads it back.

.. note::

   The sample intentionally avoids :c:func:`fs_statvfs`. On FatFs it calls
   ``f_getfree()``, which scans the entire FAT to count free clusters whenever
   the drive's FSInfo free-count is stale. Over USB — one SCSI ``READ(10)`` per
   sector — scanning a multi-gigabyte volume's FAT takes tens of seconds and
   looks like a hang. The drive's total capacity is already reported by
   ``READ CAPACITY`` during the SCSI probe.

.. important::

   The auto-format-on-mount-failure behaviour of FatFs is **disabled** in this
   sample (``CONFIG_FS_FATFS_MOUNT_MKFS=n`` plus ``FS_MOUNT_FLAG_NO_FORMAT``).
   An unreadable or non-FAT drive is reported, never reformatted, so existing
   data on the stick is preserved.

Requirements
************

* Alif Ensemble E7 or E8 Development Kit
* A USB flash drive (High Speed, SCSI transparent / Bulk-Only Transport)
* USB cable connecting the drive to the DK's USB host port

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
     alif/samples/drivers/usb/usb_mass_storage \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Build for the E8 DK HE core:

.. code-block:: console

   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_he \
     alif/samples/drivers/usb/usb_mass_storage \
     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay

Flash and open the serial console.

Sample Output
*************

With a USB flash drive plugged in:

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   [00:00:00.000,000] <inf> usb_msc_sample: USB Host Mass Storage Sample
   [00:00:00.000,000] <inf> usb_msc_sample: ============================
   [00:00:00.000,000] <inf> usb_msc_sample: UHC device ready: usb@48200000
   [00:00:00.106,000] <inf> usb_msc_sample: UHC enabled - plug a USB mass storage device into the USB port
   [00:00:02.606,000] <inf> usb_msc_sample: Device detected, starting setup...
   ... driver enumeration logs (port reset, enable slot, address device) ...
   [00:00:03.500,000] <inf> usbh_msc: MSC ready: 'JetFlash' 'Transcend 16GB  ' rev '1.00', 30867456 blocks x 512 B (disk 'USB')
   [00:00:03.510,000] <inf> usb_msc_sample: Mass storage ready:
   [00:00:03.510,000] <inf> usb_msc_sample:   VID=0x8564 PID=0x1000
   [00:00:03.510,000] <inf> usb_msc_sample:   JetFlash Transcend 16GB   rev 1.00
   [00:00:03.510,000] <inf> usb_msc_sample:   30867456 blocks x 512 B (15072 MB)
   [00:00:03.600,000] <inf> usb_msc_sample: === READ(10) LBA 0 ===
   [00:00:03.620,000] <inf> usb_msc_sample: Read 512 bytes from LBA 0
   [00:00:03.620,000] <inf> usb_msc_sample: LBA 0 (first 64 bytes)
                                            33 c0 8e d0 bc 00 7c 8e ...
   [00:00:03.620,000] <inf> usb_msc_sample:   Valid boot-sector signature (0x55AA) present
   [00:00:03.630,000] <inf> usb_msc_sample: === Mounting FAT filesystem at /USB: ===
   [00:00:03.700,000] <inf> usb_msc_sample: Filesystem mounted
   [00:00:03.720,000] <inf> usb_msc_sample: === Looking for /USB:/HELLO.TXT ===
   [00:00:03.740,000] <inf> usb_msc_sample: File not found; creating it
   [00:00:03.900,000] <inf> usb_msc_sample: Created /USB:/HELLO.TXT (139 bytes)
   [00:00:03.920,000] <inf> usb_msc_sample:   Path: /USB:/HELLO.TXT
   [00:00:03.920,000] <inf> usb_msc_sample:   Size: 139 bytes
   [00:00:03.920,000] <inf> usb_msc_sample:   First 139 bytes (up to 5 lines):
       | Hello from Zephyr USB host mass storage!
       | This file was created by the usb_mass_storage sample
       | running on an Alif Ensemble DK in USB host mode.
   [00:00:03.930,000] <inf> usb_msc_sample: Filesystem unmounted

On a second run (or after re-inserting the drive) the file already exists, so
the sample prints its details and first lines instead of creating it.

Implementation Details
**********************

The DT overlay (``boards/alif_usb_host.overlay``) switches the USB node's
compatible from ``snps,dwc3`` (device mode) to ``snps,dwc3-host`` (host mode),
enables USB PHY power via AIPM, and creates the ``zephyr_uhc0`` node label.

The USB Mass Storage protocol is implemented by the reusable host class driver
``zephyr/subsys/usb/host/usbh_msc.c`` (public API in
``zephyr/include/zephyr/usb/host/msc.h``), enabled with
:kconfig:option:`CONFIG_USBH_MSC`. The driver builds each SCSI command as a
CBW, sends it, runs the optional data phase, and reads the CSW using the UHC
DWC3 driver's bulk transfer helpers (``uhc_dwc3_bulk_out`` /
``uhc_dwc3_bulk_in``); enumeration and endpoint configuration are done by
``uhc_dwc3_setup_device``. It currently uses those DWC3 synchronous helpers, so
it depends on :kconfig:option:`CONFIG_UHC_DWC3`.

The driver also provides the disk-access back end: a ``struct disk_operations``
whose ``read`` / ``write`` callbacks issue SCSI ``READ(10)`` / ``WRITE(10)``
commands (one 512-byte block per command so each transfer stays within the
controller's bulk buffer), registered on probe under
:kconfig:option:`CONFIG_USBH_MSC_DISK_NAME`. The sample mounts that disk at
``/USB:`` via :c:func:`fs_mount` and uses the standard :c:func:`fs_stat`,
:c:func:`fs_open`, :c:func:`fs_read`, :c:func:`fs_write` and
:c:func:`fs_close` APIs.

Because typical flash drives enumerate at High Speed, their bulk endpoints use
a 512-byte max packet size and logical blocks are 512 bytes. The UHC DWC3
driver programs the xHCI endpoint context with the max packet size reported in
the endpoint descriptor and keeps 512-byte bulk DMA buffers so a full block can
be transferred in a single request.

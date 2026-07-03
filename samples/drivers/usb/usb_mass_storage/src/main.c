/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Host Mass Storage Sample
 *
 * This sample brings up the DWC3 USB controller in host mode and uses the USB
 * host Mass Storage class driver (:c:func:`usbh_msc_probe`) to enumerate a
 * connected USB flash drive, run the SCSI discovery sequence and register a
 * disk-access device. All of the Bulk-Only Transport / SCSI protocol lives in
 * the reusable driver (``zephyr/subsys/usb/host/usbh_msc.c``); this sample only
 * drives it and demonstrates a filesystem on top:
 *
 *   1. Wait for a device to be connected
 *   2. usbh_msc_probe(): enumerate + SCSI INQUIRY / READ CAPACITY + register disk
 *   3. Dump the first logical block (READ(10) of LBA 0)
 *   4. Mount the FAT/vfat filesystem and look for HELLO.TXT, printing it or
 *      creating it with example text when missing
 *
 * How to test:
 *   1. Build for E8 DK HP or HE core with the overlay
 *   2. Flash and open the serial console
 *   3. Plug a USB flash drive into the DK USB host port
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/host/msc.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/fs/fs.h>
#include <ff.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_msc_sample, LOG_LEVEL_INF);

/* Mass storage instance managed by the USB host MSC class driver. */
static struct usbh_msc_dev msc;

/* Scratch buffer for the LBA 0 dump (one logical block). */
static uint8_t read_buf[512];

/* ---- FAT filesystem demo ----
 *
 * The MSC driver registers the medium under CONFIG_USBH_MSC_DISK_NAME ("USB"),
 * which maps to the FatFs volume "/USB:".
 */
#define FS_MOUNT_POINT	"/USB:"
#define DEMO_FILE_PATH	FS_MOUNT_POINT "/HELLO.TXT"

static FATFS fat_fs;
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = FS_MOUNT_POINT,
	/* Never reformat the user's stick if the mount fails. */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

static const char demo_content[] =
	"Hello from Zephyr USB host mass storage!\r\n"
	"This file was created by the usb_mass_storage sample\r\n"
	"running on an Alif Ensemble DK in USB host mode.\r\n";

/* Print the file's size and its first few lines of text. */
static void print_file_details(const char *path, size_t size)
{
	struct fs_file_t file;
	char buf[192];
	ssize_t n;
	int err;

	LOG_INF("  Path: %s", path);
	LOG_INF("  Size: %zu bytes", size);

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		LOG_ERR("fs_open(%s) failed: %d", path, err);
		return;
	}

	n = fs_read(&file, buf, sizeof(buf) - 1);
	fs_close(&file);

	if (n < 0) {
		LOG_ERR("fs_read failed: %d", (int)n);
		return;
	}
	buf[n] = '\0';

	LOG_INF("  First %d bytes (up to 5 lines):", (int)n);

	const char *p = buf;
	int line = 0;

	while (*p && line < 5) {
		const char *nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);

		if (len > 0 && p[len - 1] == '\r') {
			len--; /* drop trailing CR for tidy output */
		}
		printk("    | %.*s\n", len, p);

		if (!nl) {
			break;
		}
		p = nl + 1;
		line++;
	}
}

static int create_demo_file(const char *path)
{
	struct fs_file_t file;
	ssize_t n;
	int err;
	int cerr;

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (err) {
		LOG_ERR("fs_open(create %s) failed: %d", path, err);
		return err;
	}

	n = fs_write(&file, demo_content, sizeof(demo_content) - 1);
	cerr = fs_close(&file);

	if (n < 0) {
		LOG_ERR("fs_write failed: %d", (int)n);
		return (int)n;
	}
	if (cerr) {
		LOG_ERR("fs_close failed: %d", cerr);
		return cerr;
	}
	if (n != (ssize_t)(sizeof(demo_content) - 1)) {
		LOG_ERR("Short write: %d of %zu", (int)n, sizeof(demo_content) - 1);
		return -EIO;
	}

	return 0;
}

/*
 * Mount the FAT/vfat filesystem on the stick, check for a specific file and
 * print it, or create it if missing. USB flash drives are almost always
 * MBR-partitioned with a single FAT volume; FatFs reads the MBR and mounts the
 * first partition automatically.
 */
static void msc_fs_demo(void)
{
	struct fs_dirent entry;
	int err;

	LOG_INF("=== Mounting FAT filesystem at %s ===", FS_MOUNT_POINT);
	err = fs_mount(&fatfs_mnt);
	if (err == -ENODEV) {
		LOG_WRN("No FAT/vfat filesystem found on the device");
		return;
	} else if (err) {
		LOG_ERR("fs_mount failed: %d", err);
		return;
	}
	LOG_INF("Filesystem mounted");

	/*
	 * Note: we deliberately do NOT call fs_statvfs() here. On FatFs it runs
	 * f_getfree(), which scans the whole FAT to count free clusters when the
	 * drive's FSInfo free-count is stale. Over USB, at one SCSI READ(10) per
	 * sector, scanning a multi-GB volume's FAT takes tens of seconds and
	 * looks like a hang. The drive's total capacity is already reported by
	 * READ CAPACITY above.
	 */

	LOG_INF("=== Looking for %s ===", DEMO_FILE_PATH);
	err = fs_stat(DEMO_FILE_PATH, &entry);
	if (err == 0) {
		LOG_INF("File exists (%s)",
			entry.type == FS_DIR_ENTRY_DIR ? "directory" : "file");
		print_file_details(DEMO_FILE_PATH, entry.size);
	} else if (err == -ENOENT) {
		LOG_INF("File not found; creating it");
		err = create_demo_file(DEMO_FILE_PATH);
		if (err == 0) {
			LOG_INF("Created %s (%zu bytes)", DEMO_FILE_PATH,
				sizeof(demo_content) - 1);
			print_file_details(DEMO_FILE_PATH,
					   sizeof(demo_content) - 1);
		}
	} else {
		LOG_ERR("fs_stat(%s) failed: %d", DEMO_FILE_PATH, err);
	}

	err = fs_unmount(&fatfs_mnt);
	if (err) {
		LOG_ERR("fs_unmount failed: %d", err);
	} else {
		LOG_INF("Filesystem unmounted");
	}
}

/* Dump the first logical block (LBA 0) via the MSC driver. */
static void dump_first_block(void)
{
	int ret;

	if (msc.block_size == 0 || msc.block_size > sizeof(read_buf)) {
		LOG_WRN("Block size %u unsupported for dump", msc.block_size);
		return;
	}

	LOG_INF("=== READ(10) LBA 0 ===");
	ret = usbh_msc_read(&msc, 0, 1, read_buf);
	if (ret < 0) {
		LOG_ERR("READ(10) failed: %d", ret);
		return;
	}

	LOG_INF("Read %d bytes from LBA 0", ret);
	LOG_HEXDUMP_INF(read_buf, 64, "LBA 0 (first 64 bytes)");

	if (msc.block_size >= 512 &&
	    read_buf[510] == 0x55 && read_buf[511] == 0xAA) {
		LOG_INF("  Valid boot-sector signature (0x55AA) present");
	}
}

/* Get the UHC device from devicetree - the overlay creates the zephyr_uhc0 label */
#define UHC_NODE DT_NODELABEL(zephyr_uhc0)

#if !DT_NODE_EXISTS(UHC_NODE)
#error "zephyr_uhc0 node not found. Check your overlay applies snps,dwc3-host compatible."
#endif

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);

USBH_CONTROLLER_DEFINE(sample_uhs_ctx, DEVICE_DT_GET(UHC_NODE));

/* Event callback - receives UHC events */
static int usb_host_event_cb(const struct device *dev,
			     const struct uhc_event *const event)
{
	ARG_UNUSED(dev);

	switch (event->type) {
	case UHC_EVT_DEV_CONNECTED_FS:
		LOG_INF(">>> USB device connected (Full Speed)");
		break;
	case UHC_EVT_DEV_CONNECTED_HS:
		LOG_INF(">>> USB device connected (High Speed)");
		break;
	case UHC_EVT_DEV_REMOVED:
		LOG_INF(">>> USB device disconnected");
		break;
	case UHC_EVT_RESETED:
		LOG_INF("Bus reset completed");
		break;
	case UHC_EVT_ERROR:
		LOG_ERR("USB host error: %d", event->status);
		break;
	default:
		break;
	}

	return 0;
}

int main(void)
{
	int err;
	bool done = false;

	LOG_INF("USB Host Mass Storage Sample");
	LOG_INF("============================");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("USB host controller device not ready");
		return -ENODEV;
	}
	LOG_INF("UHC device ready: %s", uhc_dev->name);

	err = uhc_init(uhc_dev, usb_host_event_cb, &sample_uhs_ctx);
	if (err) {
		LOG_ERR("Failed to initialize UHC: %d", err);
		return err;
	}

	err = uhc_enable(uhc_dev);
	if (err) {
		LOG_ERR("Failed to enable UHC: %d", err);
		return err;
	}

	LOG_INF("UHC enabled - plug a USB mass storage device into the USB port");

	while (true) {
		k_sleep(done ? K_MSEC(500) : K_SECONDS(2));

		volatile uint32_t *portsc =
			(volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0x420);
		bool connected = !!(*portsc & BIT(0));

		if (connected && !done) {
			LOG_INF("Device detected, starting setup...");
			k_sleep(K_MSEC(500)); /* Debounce / device settle */

			err = usbh_msc_probe(uhc_dev, &msc);
			if (err) {
				LOG_ERR("Mass storage probe failed: %d", err);
				LOG_INF("Will retry in 10 seconds...");
				k_sleep(K_SECONDS(10));
				continue;
			}

			uint32_t capacity_mb = (uint32_t)
				(((uint64_t)msc.block_count * msc.block_size) /
				 (1024ULL * 1024ULL));

			LOG_INF("Mass storage ready:");
			LOG_INF("  VID=0x%04x PID=0x%04x", msc.vid, msc.pid);
			LOG_INF("  %s %s rev %s",
				msc.vendor, msc.product, msc.revision);
			LOG_INF("  %u blocks x %u B (%u MB)",
				msc.block_count, msc.block_size, capacity_mb);

			dump_first_block();
			msc_fs_demo();

			done = true;
		} else if (!connected && done) {
			LOG_INF("Device disconnected");
			usbh_msc_remove(&msc);
			done = false;
		}
	}

	return 0;
}

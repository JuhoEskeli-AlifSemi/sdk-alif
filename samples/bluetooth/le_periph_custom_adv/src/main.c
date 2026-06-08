/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/*
 * Demonstration: Custom BLE MAC Address and Serial Number in Advertisement
 *
 * This sample shows how to:
 * 1. Set a custom (fixed) static random MAC address for BLE advertising
 * 2. Include a serial number in the manufacturer-specific advertisement data
 *
 * The custom MAC address is set directly in the gapm_config_t structure.
 * The serial number is broadcast as part of the manufacturer-specific data
 * in the advertisement packet, visible to any BLE scanner without connecting.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "address_verification.h"
#include <alif/bluetooth/bt_adv_data.h>
#include <alif/bluetooth/bt_scan_rsp.h>
#include "gapm_api.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* Load name from configuration file */
#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME

/*
 * ============================================================================
 * CUSTOM MAC ADDRESS CONFIGURATION
 * ============================================================================
 * Set your desired custom BLE address here.
 * For a static random address, the two most significant bits of the
 * last byte (addr[5]) must be set to 1 (i.e., addr[5] |= 0xC0).
 *
 * Example: address will appear as C1:22:33:44:55:66 on BLE scanners.
 */
#define CUSTOM_BLE_ADDR {0x66, 0x55, 0x44, 0x33, 0x22, 0xC1}

/*
 * ============================================================================
 * SERIAL NUMBER CONFIGURATION
 * ============================================================================
 * This serial number will be included in the manufacturer-specific
 * advertisement data and visible to BLE scanners without connecting.
 *
 * Format: raw bytes following the company ID in the advertisement.
 * You can encode any serial number format here.
 */
static const uint8_t custom_serial_number[] = {
	'S', 'N', '-',                    /* Prefix */
	'A', 'L', 'I', 'F', '-',         /* Manufacturer prefix */
	'2', '0', '2', '5', '0', '0', '1' /* Serial digits */
};

/* Store advertising activity index for re-starting after disconnection */
static uint8_t adv_actv_idx;

/**
 * Bluetooth stack configuration with CUSTOM MAC address
 */
static gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_PERIPHERAL,
	.pairing_mode = GAPM_PAIRING_DISABLE,
	.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT, /* Required for static random address */
	.renew_dur = 1500,
	.private_identity.addr = CUSTOM_BLE_ADDR,    /* <-- Custom MAC address set here */
	.irk.key = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = 0,
	.sugg_max_tx_octets = GAP_LE_MIN_OCTETS,
	.sugg_max_tx_time = GAP_LE_MIN_TIME,
	.tx_pref_phy = GAP_PHY_ANY,
	.rx_pref_phy = GAP_PHY_ANY,
	.tx_path_comp = 0,
	.rx_path_comp = 0,
	.class_of_device = 0,  /* BT Classic only */
	.dflt_link_policy = 0, /* BT Classic only */
};

/*
 * We use GAPM_STATIC_ADDR directly instead of calling address_verification()
 * so the custom address in gapm_cfg is NOT overwritten with a random one.
 */
static uint8_t adv_type = GAPM_STATIC_ADDR;

static uint16_t set_advertising_data(uint8_t actv_idx)
{
	int ret;
	uint16_t comp_id = CONFIG_BLE_COMPANY_ID;

	/*
	 * Set manufacturer-specific data containing the serial number.
	 * In the advertisement packet this appears as:
	 *   AD Type: 0xFF (Manufacturer Specific Data)
	 *   Company ID: CONFIG_BLE_COMPANY_ID (2 bytes, little-endian)
	 *   Data: custom_serial_number bytes
	 *
	 * Any BLE scanner will see this without needing to connect.
	 */
	ret = bt_adv_data_set_manufacturer(comp_id, custom_serial_number,
					   sizeof(custom_serial_number));
	if (ret) {
		LOG_ERR("AD manufacturer data fail %d", ret);
		return ATT_ERR_INSUFF_RESOURCE;
	}

	/* Set the device name in advertisement */
	ret = bt_adv_data_set_name_auto(DEVICE_NAME, strlen(DEVICE_NAME));
	if (ret) {
		LOG_ERR("AD device name data fail %d", ret);
		return ATT_ERR_INSUFF_RESOURCE;
	}

	return bt_gapm_advertiment_data_set(actv_idx);
}

static uint16_t create_advertising(void)
{
	gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
		.tx_pwr = 0,
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
			.adv_intv_min = 160, /* 100 ms */
			.adv_intv_max = 800, /* 500 ms */
			.ch_map = ADV_ALL_CHNLS_EN,
			.phy = GAPM_PHY_TYPE_LE_1M,
		},
	};

	return bt_gapm_le_create_advertisement_service(adv_type, &adv_create_params, NULL,
						       &adv_actv_idx);
}

static void app_connection_status_update(enum gapm_connection_event const con_event,
					 uint8_t const con_idx, uint16_t const status)
{
	switch (con_event) {
	case GAPM_API_DEV_CONNECTED:
		LOG_INF("Device connected (conidx: %u)", con_idx);
		break;
	case GAPM_API_DEV_DISCONNECTED:
		LOG_INF("Device disconnected (conidx: %u, reason: %u)", con_idx, status);
		/* Restart advertising after disconnection */
		bt_gapm_advertisement_start(adv_actv_idx);
		break;
	default:
		break;
	}
}

static gapm_user_cb_t gapm_user_cb = {
	.connection_status_update = app_connection_status_update,
};

int main(void)
{
	uint16_t err;

	LOG_INF("=== Custom BLE Address & Serial Number Demo ===");
	LOG_INF("Custom Address: %02X:%02X:%02X:%02X:%02X:%02X",
		gapm_cfg.private_identity.addr[5],
		gapm_cfg.private_identity.addr[4],
		gapm_cfg.private_identity.addr[3],
		gapm_cfg.private_identity.addr[2],
		gapm_cfg.private_identity.addr[1],
		gapm_cfg.private_identity.addr[0]);
	LOG_INF("Serial Number: %.*s", (int)sizeof(custom_serial_number), custom_serial_number);

	/* Start up bluetooth host stack */
	alif_ble_enable(NULL);

	/*
	 * NOTE: We skip address_verification() here intentionally.
	 * Calling address_verification(ALIF_STATIC_RAND_ADDR, ...) would overwrite
	 * our custom address with a random one. Instead, we set:
	 *   - gapm_cfg.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT  (already set above)
	 *   - adv_type = GAPM_STATIC_ADDR                          (already set above)
	 *   - gapm_cfg.private_identity.addr = our custom address  (already set above)
	 */

	/* Configure Bluetooth Stack */
	LOG_INF("Initializing BLE stack...");
	err = bt_gapm_init(&gapm_cfg, &gapm_user_cb, DEVICE_NAME, strlen(DEVICE_NAME));
	if (err) {
		LOG_ERR("bt_gapm_init error %u", err);
		return -1;
	}

	/* Create advertisement activity */
	err = create_advertising();
	if (err) {
		LOG_ERR("Advertisement create fail %u", err);
		return -1;
	}

	/* Set advertisement data (includes serial number in manufacturer data) */
	err = set_advertising_data(adv_actv_idx);
	if (err) {
		LOG_ERR("Advertisement data set fail %u", err);
		return -1;
	}

	/* Set scan response data */
	err = bt_gapm_scan_response_set(adv_actv_idx);
	if (err) {
		LOG_ERR("Scan response set fail %u", err);
		return -1;
	}

	/* Start advertising */
	err = bt_gapm_advertisement_start(adv_actv_idx);
	if (err) {
		LOG_ERR("Advertisement start fail %u", err);
		return -1;
	}

	/* Print the actual advertising address */
	print_device_identity();
	address_verification_log_advertising_address(adv_actv_idx);

	LOG_INF("Advertising started. Scan with nRF Connect or similar to see:");
	LOG_INF("  - Custom MAC address: C1:22:33:44:55:66");
	LOG_INF("  - Manufacturer data containing serial: SN-ALIF-2025001");

	/* Keep running */
	while (1) {
		k_sleep(K_SECONDS(10));
		LOG_DBG("Still advertising...");
	}
}

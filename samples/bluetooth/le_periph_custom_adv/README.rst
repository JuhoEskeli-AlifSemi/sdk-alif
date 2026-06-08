.. _le_periph_custom_adv:

BLE Custom MAC Address and Serial Number Advertisement
######################################################

Overview
********

This sample demonstrates how to:

1. **Set a custom (fixed) BLE MAC address** for advertising
2. **Include a serial number in the advertisement data** (visible to scanners without connecting)

The custom address and serial number are configurable at compile time via macros in ``src/main.c``.

How It Works
************

Custom MAC Address
==================

The BLE address is set directly in the ``gapm_config_t`` structure:

.. code-block:: c

   #define CUSTOM_BLE_ADDR {0x66, 0x55, 0x44, 0x33, 0x22, 0xC1}

   static gapm_config_t gapm_cfg = {
       ...
       .privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT,
       .private_identity.addr = CUSTOM_BLE_ADDR,
       ...
   };

**Important:** For a static random address, the two most significant bits of byte
``addr[5]`` must be set to ``1`` (i.e., ``0xC0`` or higher in the MSB). This is a
Bluetooth specification requirement.

The sample intentionally skips calling ``address_verification()`` because that function
would overwrite the custom address with a random one when using ``ALIF_STATIC_RAND_ADDR``.

Serial Number in Advertisement
==============================

The serial number is included as manufacturer-specific data in the advertisement packet:

.. code-block:: c

   static const uint8_t custom_serial_number[] = {
       'S', 'N', '-',
       'A', 'L', 'I', 'F', '-',
       '2', '0', '2', '5', '0', '0', '1'
   };

   bt_adv_data_set_manufacturer(comp_id, custom_serial_number,
                                sizeof(custom_serial_number));

This data is visible to any BLE scanner (e.g., nRF Connect) without needing to establish
a connection. It appears under "Manufacturer Specific Data" in the advertisement.

Configuration
*************

To change the custom MAC address, modify ``CUSTOM_BLE_ADDR`` in ``src/main.c``.

To change the serial number, modify the ``custom_serial_number`` array in ``src/main.c``.

To change the device name, modify ``CONFIG_BLE_DEVICE_NAME`` in ``prj.conf``.

Building and Running
********************

.. code-block:: console

   # For B1 EB:
   west build -b alif_b1_eb/ab1c1f4m51820hh0/rtss_he alif/samples/bluetooth/le_periph_custom_adv
   # For B1 DK:
   west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he alif/samples/bluetooth/le_periph_custom_adv

   west flash

Testing
*******

1. Build and flash the sample.
2. Open a BLE scanner app (e.g., nRF Connect on mobile).
3. Verify:
   - The device appears with address ``C1:22:33:44:55:66``
   - The device name shows as ``ALIF_CUSTOM``
   - Under advertisement data, "Manufacturer Specific Data" contains the serial number bytes

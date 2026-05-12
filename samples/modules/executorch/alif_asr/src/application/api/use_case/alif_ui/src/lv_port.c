/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/* Zephyr implementation of the MLEK lv_port interface.
 * lv_port_disp_init() is a no-op because Zephyr initialises LVGL.
 * lv_port_lock()/lv_port_unlock() use a k_mutex to serialise LVGL access.
 */

#include "lv_port.h"
#include <zephyr/kernel.h>

K_MUTEX_DEFINE(lvgl_mutex);

void lv_port_disp_init(void)
{
    /* Intentionally empty: Zephyr LVGL subsystem handles display init. */
}

uint32_t lv_port_lock(void)
{
    k_mutex_lock(&lvgl_mutex, K_FOREVER);
    return 0;
}

void lv_port_unlock(uint32_t state)
{
    (void)state;
    k_mutex_unlock(&lvgl_mutex);
}

/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/* Zephyr-compatible drop-in replacement for the MLEK baremetal lv_port.h.
 * Locking uses a Zephyr k_mutex instead of interrupt masking.
 */

#ifndef LV_PORT_H_
#define LV_PORT_H_

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#if LV_COLOR_DEPTH == 32
typedef lv_color32_t lvgl_pixel_t;
#elif LV_COLOR_DEPTH == 16
typedef lv_color16_t lvgl_pixel_t;
#else
#error "Unsupported LV_COLOR_DEPTH"
#endif

/* No-op on Zephyr — LVGL is initialised by the Zephyr LVGL subsystem. */
void lv_port_disp_init(void);

/* Acquire the LVGL mutex.  Returns 0 (no interrupt state to restore). */
uint32_t lv_port_lock(void);

/* Release the LVGL mutex.  The state argument is ignored on Zephyr. */
void lv_port_unlock(uint32_t state);

#ifdef __cplusplus
}

class ScopedLVGLLock {
private:
    uint32_t m_oldState;
public:
    ScopedLVGLLock() {
        m_oldState = lv_port_lock();
    }
    ~ScopedLVGLLock() {
        lv_port_unlock(m_oldState);
    }
    ScopedLVGLLock(const ScopedLVGLLock &) = delete;
    ScopedLVGLLock &operator=(const ScopedLVGLLock &) = delete;
};

#endif /* __cplusplus */

#endif /* LV_PORT_H_ */

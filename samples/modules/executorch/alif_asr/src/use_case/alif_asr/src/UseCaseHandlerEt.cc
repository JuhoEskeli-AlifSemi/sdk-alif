/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/*
 * SPDX-FileCopyrightText: Copyright 2025 Arm Limited and/or its
 * affiliates <open-source-office@arm.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/* Zephyr adaptation of the MLEK alif_asr UseCaseHandlerEt.cc.
 * - Audio: Zephyr I2S AudioBackend (audio_init / get_audio_data / etc.)
 * - Button / LED: Zephyr GPIO (sw0 / led0 aliases)
 * - LVGL locking: lv_port_lock / ScopedLVGLLock via local Zephyr shim
 * - Timing: k_cycle_get_32() / sys_clock_hw_cycles_per_sec()
 * - Profiler omitted (requires MLEK HAL PMU unavailable in Zephyr)
 */

#include "UseCaseHandler.hpp"
#include "AudioBackend.hpp"
#include "AppContext.hpp"

#include "mlek/use_case/asr/ConformerProcessing.hpp"
#include "mlek/log/log_macros.h"

#include "ScreenLayout.hpp"
#include "lv_paint_utils.h"
#include "lv_port.h"
#include "lvgl.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <cstring>
#include <cmath>

LOG_MODULE_REGISTER(UseCaseHandler);

#define AUDIO_RATE               (16000)
#define AUDIO_CHUNK_SIZE_SAMPLES (2048)
#define AUDIO_CHUNKS             (78)   /* ~10 s max recording */
#define AUDIO_SAMPLES            (AUDIO_CHUNK_SIZE_SAMPLES * AUDIO_CHUNKS)

#define LIMAGE_X  (340)
#define LIMAGE_Y  (80)
#define LV_ZOOM   (int)(1.2 * 256)

namespace {
lv_style_t boxStyle;
lvgl_pixel_t lvgl_image[LIMAGE_Y][LIMAGE_X] __attribute__((section(".bss.lcd_image_buf")));
}

static int16_t audio_inf[AUDIO_SAMPLES + AUDIO_CHUNK_SIZE_SAMPLES];

static const int result_label_idx = 4;

/* ------------------------------------------------------------------ */
/* Button / LED                                                         */
/* ------------------------------------------------------------------ */
static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct gpio_callback button_cb_data;

static volatile bool button_pressed;
static volatile int  button_pressed_sample_count;

static void button_isr(const struct device *dev, struct gpio_callback *cb,
                        uint32_t pins)
{
    (void)dev; (void)cb; (void)pins;
    button_pressed              = true;
    button_pressed_sample_count = get_audio_samples_received();
}

static int board_init(void)
{
    if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&led)) {
        LOG_ERR("GPIO devices not ready");
        return -ENODEV;
    }

    int rc = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (rc) { LOG_ERR("button configure failed: %d", rc); return rc; }

    rc = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_FALLING);
    if (rc) { LOG_ERR("button interrupt configure failed: %d", rc); return rc; }

    gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    rc = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (rc) { LOG_ERR("LED configure failed: %d", rc); return rc; }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Mel-spectrogram visualisation                                        */
/* ------------------------------------------------------------------ */
static void getHeatMapColor(float value, float *r, float *g, float *b)
{
    static const float color[4][3] = {
        {0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}
    };
    int idx1, idx2;
    float frac = 0.0f;

    if (value <= 0.0f) {
        idx1 = idx2 = 0;
    } else if (value >= 1.0f) {
        idx1 = idx2 = 3;
    } else {
        value = value * 3.0f;
        idx1  = (int)floorf(value);
        idx2  = idx1 + 1;
        frac  = value - (float)idx1;
    }

    *r = (color[idx2][0] - color[idx1][0]) * frac + color[idx1][0];
    *g = (color[idx2][1] - color[idx1][1]) * frac + color[idx1][1];
    *b = (color[idx2][2] - color[idx1][2]) * frac + color[idx1][2];
}

static void drawMelSpec(arm::app::fwk::iface::TensorIface &inputMelSpec)
{
    const int   input_channels       = inputMelSpec.Shape()[2];
    const int   input_visualize_step = 3;
    float      *mel_input            = (float *)inputMelSpec.GetData();

    for (int xx = 0; xx < LIMAGE_X; xx++) {
        for (int yy = 0; yy < LIMAGE_Y; yy++) {
            float mel_value =
                (mel_input[(xx * input_visualize_step * input_channels) + yy] + 1.0f) / 2.0f;

            float fr, fg, fb;
            getHeatMapColor(mel_value, &fr, &fg, &fb);

            lv_color16_t rgb = {
                static_cast<uint16_t>((uint16_t)(fb * 255) >> 3),
                static_cast<uint16_t>((uint16_t)(fg * 255) >> 2),
                static_cast<uint16_t>((uint16_t)(fr * 255) >> 3)
            };
            lvgl_image[yy][xx] = rgb;
        }
    }
    {
        lv_obj_invalidate(alif::app::ScreenLayoutImageObject());
        ScopedLVGLLock lv_lock;
    }
}

/* ------------------------------------------------------------------ */
/* Use-case entry points                                                */
/* ------------------------------------------------------------------ */
static bool ClassifyAudioInit(void)
{
    alif::app::ScreenLayoutInit(lvgl_image, sizeof(lvgl_image),
                                LIMAGE_X, LIMAGE_Y, LV_ZOOM, true);
    std::memset(lvgl_image, 0, sizeof(lvgl_image));

    uint32_t lv_lock_state = lv_port_lock();

    lv_label_set_text_static(alif::app::ScreenLayoutHeaderObject(),
                             "Conformer ASR (ExecuTorch)");

    lv_style_init(&boxStyle);
    lv_style_set_bg_opa(&boxStyle, LV_OPA_TRANSP);
    lv_style_set_pad_all(&boxStyle, 0);
    lv_style_set_border_width(&boxStyle, 0);
    lv_style_set_outline_width(&boxStyle, 2);
    lv_style_set_outline_pad(&boxStyle, 0);
    lv_style_set_outline_color(
        &boxStyle, lv_theme_get_color_primary(alif::app::ScreenLayoutHeaderObject()));
    lv_style_set_radius(&boxStyle, 4);

    lv_obj_add_flag(alif::app::ScreenLayoutBarObject(), LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_static(alif::app::ScreenLayoutLabelObject(0), "");
    lv_label_set_text_static(alif::app::ScreenLayoutLabelObject(result_label_idx), "");
    lv_obj_set_width(alif::app::ScreenLayoutLabelObject(result_label_idx), 460);
    lv_label_set_long_mode(alif::app::ScreenLayoutLabelObject(result_label_idx),
                           LV_LABEL_LONG_WRAP);

    lv_port_unlock(lv_lock_state);

    return board_init() == 0;
}

namespace arm {
namespace app {

bool ClassifyAudioHandler(ApplicationContext &ctx)
{
    auto &model            = ctx.Get<fwk::iface::Model &>("model");
    auto &labels           = ctx.Get<const std::vector<std::string> &>("labels");
    auto  melSpecWindowSize = ctx.Get<uint32_t>("melSpecWindowSize");
    auto  melSpecHopSize    = ctx.Get<uint32_t>("melSpecHopSize");
    auto  chunkSize         = ctx.Get<uint32_t>("chunkSize");

    if (!model.IsInited()) {
        LOG_ERR("Model is not initialised!");
        return false;
    }

    auto inputTensorMelSpec   = model.GetInputTensor(0);
    auto inputTensorChunkSize = model.GetInputTensor(1);
    auto outputTensorLogits   = model.GetOutputTensor(0);
    auto outputTensorChunkSize = model.GetOutputTensor(1);

    std::string decodedResult;

    auto preProcess = ConformerPreProcess<int16_t>(
        inputTensorMelSpec, inputTensorChunkSize,
        melSpecWindowSize, melSpecHopSize, chunkSize);

    auto postProcess = ConformerPostProcess(
        outputTensorLogits, outputTensorChunkSize,
        labels, decodedResult);

    if (!ClassifyAudioInit()) {
        LOG_ERR("ClassifyAudioInit failed");
        return false;
    }

    int err = audio_init(AUDIO_RATE);
    if (err) {
        LOG_ERR("audio_init failed: %d", err);
        return false;
    }

    /* ----------------------------------------------------------------
     * Main loop: wait for button → record → infer → display
     * ---------------------------------------------------------------- */
    while (true) {

        /* -- Idle: stream audio for AGC warmup, blink LED ------------ */
        button_pressed = false;
        uint32_t idle_count = 0;

        while (!button_pressed) {
            get_audio_data(audio_inf, AUDIO_CHUNK_SIZE_SAMPLES);

            {
                ScopedLVGLLock lv_lock;
                if (idle_count & 0x08) {
                    lv_led_on(alif::app::ScreenLayoutLEDObject());
                } else {
                    lv_led_off(alif::app::ScreenLayoutLEDObject());
                }
            }

            wait_for_audio();
            audio_preprocessing(audio_inf, AUDIO_CHUNK_SIZE_SAMPLES);
            idle_count++;
        }

        {
            ScopedLVGLLock lv_lock;
            lv_obj_remove_flag(alif::app::ScreenLayoutBarObject(), LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(alif::app::ScreenLayoutBarObject());
            for (int ii = 0; ii <= result_label_idx; ii++) {
                lv_label_set_text_static(alif::app::ScreenLayoutLabelObject(ii), "");
            }
        }

        /* Align recording start to the moment the button was pressed */
        const int start_samples =
            AUDIO_CHUNK_SIZE_SAMPLES - button_pressed_sample_count;
        int16_t *audio_inf_ptr = &audio_inf[start_samples];
        if (start_samples > 0) {
            memmove(audio_inf,
                    &audio_inf[button_pressed_sample_count],
                    start_samples * sizeof(audio_inf[0]));
        }

        /* -- Record while button held (up to AUDIO_CHUNKS chunks) ---- */
        gpio_pin_set_dt(&led, 1);
        int audio_idx = 0;

        get_audio_data(audio_inf_ptr, AUDIO_CHUNK_SIZE_SAMPLES);

        while (true) {
            int btn_val = gpio_pin_get_dt(&button);
            err = wait_for_audio();
            if (err) {
                LOG_ERR("wait_for_audio error: %d", err);
                return false;
            }

            /* Start next chunk */
            if (audio_idx < (AUDIO_CHUNKS - 1)) {
                get_audio_data(
                    audio_inf_ptr + ((audio_idx + 1) * AUDIO_CHUNK_SIZE_SAMPLES),
                    AUDIO_CHUNK_SIZE_SAMPLES);
            }

            {
                ScopedLVGLLock lv_lock;
                lv_bar_set_value(alif::app::ScreenLayoutBarObject(),
                                 100 * (audio_idx + 1) / AUDIO_CHUNKS,
                                 LV_ANIM_OFF);
                lv_obj_invalidate(alif::app::ScreenLayoutBarObject());
            }

            audio_preprocessing(
                audio_inf_ptr + (audio_idx * AUDIO_CHUNK_SIZE_SAMPLES),
                AUDIO_CHUNK_SIZE_SAMPLES);

            /* Break when button released: gpio_pin_get_dt returns 0 while pressed
             * (physical LOW, pull-up, GPIO_ACTIVE_HIGH flags=0 in DTS). */
            if (btn_val != 0) {
                break;
            }

            if (audio_idx >= (AUDIO_CHUNKS - 1)) {
                break;
            }
            audio_idx++;
        }

        gpio_pin_set_dt(&led, 0);

        int16_t  *audioArr     = audio_inf;
        uint32_t  audioArrSize = (uint32_t)(start_samples +
                                            (audio_idx + 1) * AUDIO_CHUNK_SIZE_SAMPLES);

        {
            ScopedLVGLLock lv_lock;
            lv_label_set_text_static(
                alif::app::ScreenLayoutLabelObject(result_label_idx), "");
            lv_obj_add_flag(alif::app::ScreenLayoutBarObject(), LV_OBJ_FLAG_HIDDEN);
            lv_led_on(alif::app::ScreenLayoutLEDObject());
            lv_bar_set_value(alif::app::ScreenLayoutBarObject(), 0, LV_ANIM_OFF);
            lv_obj_invalidate(
                alif::app::ScreenLayoutLabelObject(result_label_idx));
        }

        /* -- Pre-process -------------------------------------------- */
        const uint32_t ts_start_pre = k_cycle_get_32();

        if (!preProcess.DoPreProcess(audioArr, audioArrSize)) {
            LOG_ERR("Pre-processing failed.");
            return false;
        }

        const uint32_t ts_done_pre = k_cycle_get_32();
        drawMelSpec(*inputTensorMelSpec);
        const uint32_t ts_start_inference = k_cycle_get_32();

        /* -- Inference ---------------------------------------------- */
        if (!model.RunInference()) {
            LOG_ERR("Inference failed.");
            return false;
        }

        const uint32_t ts_start_post = k_cycle_get_32();

        /* -- Post-process ------------------------------------------- */
        if (!postProcess.DoPostProcess()) {
            LOG_ERR("Post-processing failed.");
            return false;
        }

        const uint32_t ts_done = k_cycle_get_32();
        const uint32_t cpu_hz  = sys_clock_hw_cycles_per_sec();

        {
            ScopedLVGLLock lv_lock;
            lv_led_off(alif::app::ScreenLayoutLEDObject());

            lv_label_set_text_fmt(
                alif::app::ScreenLayoutLabelObject(0),
                "Input: %.1fs",
                (double)audioArrSize / AUDIO_RATE);

            lv_label_set_text_fmt(
                alif::app::ScreenLayoutLabelObject(1),
                "Inference: %.2f ms",
                (double)(ts_start_post - ts_start_inference) / cpu_hz * 1000.0);

            lv_label_set_text_fmt(
                alif::app::ScreenLayoutLabelObject(2),
                "Pre: %.2fms  Post: %.2fms",
                (double)(ts_done_pre - ts_start_pre) / cpu_hz * 1000.0,
                (double)(ts_done - ts_start_post)    / cpu_hz * 1000.0);

            lv_label_set_text_static(
                alif::app::ScreenLayoutLabelObject(3), "Output:");
            lv_label_set_text(
                alif::app::ScreenLayoutLabelObject(result_label_idx),
                decodedResult.c_str());
            lv_obj_invalidate(
                alif::app::ScreenLayoutLabelObject(result_label_idx));
        }

        LOG_INF("Decoded: %s", decodedResult.c_str());
    }

    return true;
}

} /* namespace app */
} /* namespace arm */

/* This file was ported to work on Alif Semiconductor Ensemble family of devices. */

/*
 * SPDX-FileCopyrightText: Copyright Alif Semiconductor
 */

/*
 * Copyright (c) 2021-2022 Arm Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "UseCaseHandler.hpp"

#include "mlek/use_case/object_detection/DetectorPostProcessing.hpp"
#include "mlek/use_case/object_detection/DetectorPreProcessing.hpp"
#include "mlek/fwk/tflm/YoloFastestModel.hpp"

#include "image_ensemble.h"

#include <cinttypes>
#include <cstring>
#include <cmath>
#include <algorithm>

#if defined(CONFIG_LVGL)
#include "ScreenLayout.hpp"
#include <lvgl.h>
#include "lv_paint_utils.h"
#include "lv_page_flip.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(UseCaseHandler);

/*
 * The camera image is shown at twice the model input size. It is doubled up
 * while converting to RGB565 (write_rgb888_to_rgb565_buf_2x()); having LVGL
 * zoom it instead costs a software transform of every pixel on each redraw.
 */

#define ASSERT_DIVISIBLE_BY_16(V) \
    static_assert(((V) % 16) == 0, \
        "Value must be divisible by 16")

#define MIMAGE_X 192
ASSERT_DIVISIBLE_BY_16(MIMAGE_X);
#define MIMAGE_Y 192

#define LIMAGE_X        (MIMAGE_X * 2)
#define LIMAGE_Y        (MIMAGE_Y * 2)
#define LV_ZOOM         (1 * 256)

/*
 * Timing, summarised once per second with the results. All times are in
 * k_cycle_get_32() cycles and summed over the period.
 */
static struct {
    uint32_t inferences;
    uint32_t capture;   /* Waiting for and converting a camera frame */
    uint32_t show;      /* Locking LVGL and copying the frame into it */
    uint32_t pre;
    uint32_t npu;
    uint32_t post;
    uint32_t boxes;     /* Locking LVGL and updating the boxes */
    uint32_t previews;  /* Live preview: frames shown */
    uint32_t preview;   /* Live preview: converting and showing them */
    uint32_t renders;   /* lv_timer_handler() calls that drew something */
    uint32_t render;
} timing;

#if defined(CONFIG_LVGL)
static inline uint32_t CyclesToUs(uint32_t cycles, uint32_t count)
{
    return count ? k_cyc_to_us_floor32(cycles / count) : 0;
}
#endif

#if defined(CONFIG_LVGL)
K_THREAD_STACK_DEFINE(lvgl_thread_stack, 1024 * 4);
static struct k_thread lvgl_thread;
static K_MUTEX_DEFINE(lvgl_mutex);

static void lvgl_worker_thread(void*, void*, void*)
{
    uint32_t time;
    while (1)
    {
        k_mutex_lock(&lvgl_mutex, K_FOREVER);
        uint32_t start = k_cycle_get_32();
        time = lv_task_handler();
        uint32_t spent = k_cycle_get_32() - start;
        k_mutex_unlock(&lvgl_mutex);

        /* Count only the calls that redrew the screen */
        if (k_cyc_to_us_floor32(spent) > 1000) {
            timing.renders++;
            timing.render += spent;
        }

        if(time < 1) {
            time = 1;
        } else if (time > LV_DEF_REFR_PERIOD) {
            time = LV_DEF_REFR_PERIOD;
        }
        k_msleep(time);
    }
}

namespace {
lv_style_t boxStyle;
/* RGB565, LIMAGE_X x LIMAGE_Y = 294,912 bytes */
uint16_t lvgl_image[LIMAGE_Y][LIMAGE_X] __aligned(4) __attribute__((section("SRAM1.lcd_image_buf")));

#if defined(CONFIG_OD_LIVE_PREVIEW)
/*
 * Live preview: a thread takes every camera frame, shows it and offers it to
 * inference, which takes the newest one whenever it is free. The image then
 * moves at the camera frame rate; the boxes follow at the inference rate.
 */
K_THREAD_STACK_DEFINE(preview_thread_stack, 1024 * 4);
struct k_thread preview_thread;
K_MUTEX_DEFINE(frame_lock);     /* Guards inferenceFrame */
K_SEM_DEFINE(frame_ready, 0, 1);
uint8_t inferenceFrame[MIMAGE_X * MIMAGE_Y * 3] __aligned(4)
    __attribute__((section("SRAM1.inference_frame_buf")));
#endif

/* Pre-allocated pool of detection box objects to avoid per-frame alloc/free */
static constexpr int MAX_DETECTION_BOXES = 10;
static lv_obj_t *boxPool[MAX_DETECTION_BOXES] = {};
static int boxPoolSize = 0;
};
#endif /* CONFIG_LVGL */

namespace alif {
namespace app {

    using namespace arm::app;

    /**
     * @brief           Presents inference results along using the data presentation
     *                  object.
     * @param[in]       results            Vector of detection results to be displayed.
     * @return          true if successful, false otherwise.
     **/
    static bool PresentInferenceResult(const std::vector<object_detection::DetectionResult>& results);

#if defined(CONFIG_LVGL)
    /**
     * @brief           Draw boxes directly on the LCD for all detected objects.
     * @param[in]       results            Vector of detection results to be displayed.
     **/
    static void DrawDetectionBoxes(
           const std::vector<object_detection::DetectionResult>& results,
           int imgInputCols, int imgInputRows);
#endif /* CONFIG_LVGL */

#if defined(CONFIG_OD_LIVE_PREVIEW)
    static void PreviewThread(void*, void*, void*)
    {
        while (true) {
            uint8_t* frame = nullptr;

            if (get_image_data(&frame) < 0) {
                LOG_ERR("Couldn't get image data");
                k_msleep(100);
                continue;
            }

            uint32_t start = k_cycle_get_32();

            k_mutex_lock(&lvgl_mutex, K_FOREVER);
            write_rgb888_to_rgb565_buf_2x(MIMAGE_X, MIMAGE_Y, frame, &lvgl_image[0][0]);
            lv_obj_invalidate(ScreenLayoutImageObject());
            /*
             * Draw it now: waiting for LVGL's refresh timer (every
             * LV_DEF_REFR_PERIOD) drops a frame whenever two arrive
             * within one period.
             */
            lv_refr_now(NULL);
            k_mutex_unlock(&lvgl_mutex);

            /* The newest frame replaces one inference has not taken yet */
            k_mutex_lock(&frame_lock, K_FOREVER);
            memcpy(inferenceFrame, frame, sizeof(inferenceFrame));
            k_mutex_unlock(&frame_lock);
            k_sem_give(&frame_ready);

            timing.previews++;
            timing.preview += k_cycle_get_32() - start;
        }
    }
#endif /* CONFIG_OD_LIVE_PREVIEW */

    bool ObjectDetectionInit()
    {
#if defined(CONFIG_LVGL)
        LOG_DBG("ScreenLayoutInit");

        /* zero image data */
        memset(&lvgl_image[0][0], 0, sizeof(lvgl_image));

        ScreenLayoutInit(lvgl_image, sizeof lvgl_image, LIMAGE_X, LIMAGE_Y, LV_ZOOM);

        lv_label_set_text_static(ScreenLayoutHeaderObject(), "Face Detection");
        lv_label_set_text_static(ScreenLayoutLabelObject(0), "Faces Detected: 0");
        lv_label_set_text_static(ScreenLayoutLabelObject(1), "192px image (24-bit)");

        lv_style_init(&boxStyle);
        lv_style_set_bg_opa(&boxStyle, LV_OPA_TRANSP);
        lv_style_set_pad_all(&boxStyle, 0);
        lv_style_set_border_width(&boxStyle, 0);
        lv_style_set_outline_width(&boxStyle, 2);
        lv_style_set_outline_pad(&boxStyle, 0);
        lv_style_set_outline_color(&boxStyle, lv_theme_get_color_primary(ScreenLayoutHeaderObject()));
        lv_style_set_radius(&boxStyle, 4);

        k_thread_create(&lvgl_thread, lvgl_thread_stack,
                        K_THREAD_STACK_SIZEOF(lvgl_thread_stack),
                        lvgl_worker_thread,
                        NULL, NULL, NULL,
                        0, 0, K_NO_WAIT);       // TODO: priority define

	    k_thread_name_set(&lvgl_thread, "lvgl");
#endif /* CONFIG_LVGL */

        if (image_init(MIMAGE_X, MIMAGE_Y) != 0) {
            return false;
        }

#if defined(CONFIG_OD_LIVE_PREVIEW)
        k_thread_create(&preview_thread, preview_thread_stack,
                        K_THREAD_STACK_SIZEOF(preview_thread_stack),
                        PreviewThread, NULL, NULL, NULL,
                        0, 0, K_NO_WAIT);
        k_thread_name_set(&preview_thread, "preview");
#endif

        return true;
    }

    /* Object detection inference handler. */
    bool ObjectDetectionHandler(ApplicationContext& ctx)
    {
        auto& model = ctx.Get<fwk::iface::Model&>("model");
        if (!model.IsInited()) {
            LOG_ERR("Model is not initialised! Terminating processing.");
            return false;
        }

        auto inputTensor = model.GetInputTensor(0);
        auto outputTensor0 = model.GetOutputTensor(0);
        auto outputTensor1 = model.GetOutputTensor(1);

        const auto inputShape = inputTensor->Shape();
        if (inputShape.empty()) {
            LOG_ERR("Invalid input tensor dims");
            return false;
        } else if (inputShape.size() < 3) {
            LOG_ERR("Input tensor dimension should be >= 3");
            return false;
        }

        /* Get input shape for displaying the image. */
        const int inputImgCols = inputShape[fwk::tflm::YoloFastestModel::ms_inputColsIdx];
        const int inputImgRows = inputShape[fwk::tflm::YoloFastestModel::ms_inputRowsIdx];

        /* Set up pre and post-processing. */
        DetectorPreProcess preProcess = DetectorPreProcess(inputTensor, true, model.IsDataSigned());

        std::vector<object_detection::DetectionResult> results;
        const object_detection::PostProcessParams postProcessParams{
            inputImgRows,
            inputImgCols,
            object_detection::originalImageSize,
            object_detection::anchor1,
            object_detection::anchor2};
        DetectorPostProcess postProcess =
            DetectorPostProcess(outputTensor0, outputTensor1, results, postProcessParams);

        const size_t copySz = inputTensor->Bytes();
        uint32_t t0 = k_cycle_get_32();
        uint32_t t1;

#if defined(CONFIG_OD_LIVE_PREVIEW)
        /* The newest frame the preview thread offered */
        k_sem_take(&frame_ready, K_FOREVER);
        t1 = k_cycle_get_32();
        timing.capture += t1 - t0;

        k_mutex_lock(&frame_lock, K_FOREVER);
        bool preOk = preProcess.DoPreProcess(inferenceFrame, copySz);
        k_mutex_unlock(&frame_lock);
#else
        uint8_t* imageDataPtr = nullptr;
        if(get_image_data(&imageDataPtr) < 0) {
            LOG_ERR("Couldn't get image data");
            return false;
        }
        t1 = k_cycle_get_32();
        timing.capture += t1 - t0;

#if defined(CONFIG_LVGL)
        k_mutex_lock(&lvgl_mutex, K_FOREVER);
        write_rgb888_to_rgb565_buf_2x(MIMAGE_X, MIMAGE_Y, imageDataPtr, &lvgl_image[0][0]);
        lv_obj_invalidate(ScreenLayoutImageObject());
        k_mutex_unlock(&lvgl_mutex);
#endif /* CONFIG_LVGL */

        t0 = k_cycle_get_32();
        timing.show += t0 - t1;
        t1 = t0;

        /* Run the pre-processing, inference and post-processing. */
        bool preOk = preProcess.DoPreProcess(imageDataPtr, copySz);
#endif /* CONFIG_OD_LIVE_PREVIEW */

        if (!preOk) {
            LOG_ERR("Pre-processing failed.");
            return false;
        }

        t0 = k_cycle_get_32();
        timing.pre += t0 - t1;

        if (!model.RunInference()) {
            LOG_ERR("Inference failed.");
            return false;
        }

        t1 = k_cycle_get_32();
        timing.npu += t1 - t0;

        if (!postProcess.DoPostProcess()) {
            LOG_ERR("Post-processing failed.");
            return false;
        }

        t0 = k_cycle_get_32();
        timing.post += t0 - t1;

#if defined(CONFIG_LVGL)
        k_mutex_lock(&lvgl_mutex, K_FOREVER);

        lv_label_set_text_fmt(ScreenLayoutLabelObject(0), "Faces Detected: %i", results.size());

        /* Draw boxes. */
        DrawDetectionBoxes(results, inputImgCols, inputImgRows);

        k_mutex_unlock(&lvgl_mutex);
#endif /* CONFIG_LVGL */

        timing.boxes += k_cycle_get_32() - t0;
        timing.inferences++;

        if (!PresentInferenceResult(results)) {
            return false;
        }

        return true;
    }

    bool
    PresentInferenceResult(const std::vector<object_detection::DetectionResult>& results)
    {
        /* If profiling is enabled, and the time is valid. */
        LOG_DBG("Final results:");
        LOG_DBG("Total number of inferences: 1");

        for (uint32_t i = 0; i < results.size(); ++i) {
            LOG_DBG("%" PRIu32 ") (%f) -> %s {x=%d,y=%d,w=%d,h=%d}",
                 i,
                 results[i].m_normalisedVal,
                 "Detection box:",
                 results[i].m_x0,
                 results[i].m_y0,
                 results[i].m_w,
                 results[i].m_h);
        }

        /* Summary once per second; the boxes above are logged at debug level */
        static uint32_t windowStart;
        static uint32_t flipsStart;
        uint32_t now = k_uptime_get_32();
        uint32_t ms = now - windowStart;

        if (ms < 1000) {
            return true;
        }

#if defined(CONFIG_LVGL)
        uint32_t flips = lv_page_flip_count();
        uint32_t n = timing.inferences;
        uint32_t inferRate10 = n * 10000 / ms;
        uint32_t displayRate10 = (flips - flipsStart) * 10000 / ms;

        LOG_INF("%u face(s), %u.%u inferences/s, display %u.%u fps",
                (unsigned int)results.size(), inferRate10 / 10, inferRate10 % 10,
                displayRate10 / 10, displayRate10 % 10);
        LOG_INF("  per inference (us): frame %u, show %u, pre %u, NPU %u, post %u, "
                "boxes %u",
                CyclesToUs(timing.capture, n), CyclesToUs(timing.show, n),
                CyclesToUs(timing.pre, n), CyclesToUs(timing.npu, n),
                CyclesToUs(timing.post, n), CyclesToUs(timing.boxes, n));
        LOG_INF("  LVGL: %u redraws, %u us each; preview: %u frames, %u us each",
                timing.renders, CyclesToUs(timing.render, timing.renders),
                timing.previews, CyclesToUs(timing.preview, timing.previews));
        flipsStart = flips;
#else
        LOG_INF("%u face(s), %u inferences in %u ms", (unsigned int)results.size(),
                timing.inferences, ms);
#endif

        memset(&timing, 0, sizeof(timing));
        windowStart = now;

        return true;
    }

#if defined(CONFIG_LVGL)
    static void InitBoxPool(lv_obj_t *frame)
    {
        for (int i = 0; i < MAX_DETECTION_BOXES; i++) {
            boxPool[i] = lv_obj_create(frame);
            lv_obj_add_style(boxPool[i], &boxStyle, LV_PART_MAIN);
            lv_obj_add_flag(boxPool[i], LV_OBJ_FLAG_HIDDEN);
        }
        boxPoolSize = MAX_DETECTION_BOXES;
    }

    static void DrawDetectionBoxes(const std::vector<object_detection::DetectionResult>& results,
                                   int imgInputCols, int imgInputRows)
    {
        lv_obj_t *frame = ScreenLayoutImageHolderObject();
        float xScale = (float) lv_obj_get_content_width(frame) / imgInputCols;
        float yScale = (float) lv_obj_get_content_height(frame) / imgInputRows;

        /* Lazily create the pool on first use */
        if (boxPoolSize == 0) {
            InitBoxPool(frame);
        }

        int numResults = std::min((int)results.size(), MAX_DETECTION_BOXES);

        /* Update active boxes */
        for (int i = 0; i < numResults; i++) {
            lv_obj_set_size(boxPool[i],
                            (int)ceil(results[i].m_w * xScale),
                            (int)ceil(results[i].m_h * yScale));
            lv_obj_set_pos(boxPool[i],
                           (int)floor(results[i].m_x0 * xScale),
                           (int)floor(results[i].m_y0 * yScale));
            lv_obj_clear_flag(boxPool[i], LV_OBJ_FLAG_HIDDEN);
        }

        /* Hide unused boxes */
        for (int i = numResults; i < MAX_DETECTION_BOXES; i++) {
            lv_obj_add_flag(boxPool[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif /* CONFIG_LVGL */

} /* namespace app */
} /* namespace alif */

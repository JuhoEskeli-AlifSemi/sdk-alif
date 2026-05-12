/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/*
 * SPDX-FileCopyrightText: Copyright 2021, 2024-2025 Arm Limited and/or its
 * affiliates <open-source-office@arm.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/* Zephyr adaptation — ExecuTorch / Conformer path only.
 * Profiler is omitted: it requires the MLEK HAL PMU which is not available
 * in the Zephyr port.
 */

#include "Labels.hpp"
#include "UseCaseHandler.hpp"
#include "BufAttributes.hpp"
#include "AppContext.hpp"

#include "mlek/fwk/executorch/ConformerModel.hpp"
#include "mlek/log/log_macros.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(MainLoop);

namespace arm {
namespace app {
    static uint8_t activationBuf[ACTIVATION_BUF_SZ] ACTIVATION_BUF_ATTRIBUTE;

    namespace asr {
        extern uint8_t *GetModelPointer();
        extern size_t   GetModelLen();
        extern const int g_melSpecWindowSize;
        extern const int g_melSpecHopSize;
        extern const int g_chunkSize;
    } /* namespace asr */
} /* namespace app */
} /* namespace arm */

static bool VerifyTensorDimensions(const arm::app::fwk::iface::Model &model)
{
    auto inputTensor = model.GetInputTensor(0);
    if (inputTensor->Shape().empty() || inputTensor->Shape().size() < 3) {
        LOG_ERR("Invalid input tensor dims");
        return false;
    }
    auto outputTensor = model.GetOutputTensor(0);
    if (outputTensor->Shape().empty() || outputTensor->Shape().size() < 3) {
        LOG_ERR("Invalid output tensor dims");
        return false;
    }
    return true;
}

void main_loop()
{
    arm::app::fwk::et::ConformerModel model;

    arm::app::fwk::iface::MemoryRegion modelMem{arm::app::asr::GetModelPointer(),
                                                arm::app::asr::GetModelLen()};
    arm::app::fwk::iface::MemoryRegion computeMem{arm::app::activationBuf,
                                                  sizeof(arm::app::activationBuf)};

    if (!model.Init(computeMem, modelMem)) {
        LOG_ERR("Failed to initialise model");
        return;
    }

    if (!VerifyTensorDimensions(model)) {
        LOG_ERR("Model tensor dimension verification failed");
        return;
    }

    arm::app::ApplicationContext caseContext;

    std::vector<std::string> labels;
    GetLabelsVector(labels);

    caseContext.Set<arm::app::fwk::iface::Model &>("model", model);
    caseContext.Set<const std::vector<std::string> &>("labels", labels);
    caseContext.Set<uint32_t>("melSpecWindowSize",
                              static_cast<uint32_t>(arm::app::asr::g_melSpecWindowSize));
    caseContext.Set<uint32_t>("melSpecHopSize",
                              static_cast<uint32_t>(arm::app::asr::g_melSpecHopSize));
    caseContext.Set<uint32_t>("chunkSize",
                              static_cast<uint32_t>(arm::app::asr::g_chunkSize));

    bool ok = arm::app::ClassifyAudioHandler(caseContext);

    LOG_INF("Main loop terminated %s.", ok ? "successfully" : "with failure");
}

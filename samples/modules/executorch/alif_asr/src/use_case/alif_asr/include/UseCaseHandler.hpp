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

#ifndef ASR_EVT_HANDLER_HPP
#define ASR_EVT_HANDLER_HPP

#include "AppContext.hpp"

namespace arm {
namespace app {

    bool ClassifyAudioHandler(ApplicationContext& ctx);

} /* namespace app */
} /* namespace arm */

#endif /* ASR_EVT_HANDLER_HPP */

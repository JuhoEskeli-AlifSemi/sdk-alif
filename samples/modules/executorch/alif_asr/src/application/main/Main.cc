/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/*
 * SPDX-FileCopyrightText: Copyright 2021-2024 Arm Limited and/or its
 * affiliates <open-source-office@arm.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mlek/fwk/executorch/EtModel.hpp"

#include <cstdio>
#include <new>
#include <exception>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(Main);

extern void main_loop();

static void print_application_intro()
{
    LOG_INF("Conformer ASR with ExecuTorch on Ethos-U85");
    LOG_INF("Build date: " __DATE__ " @ " __TIME__);
}

static void out_of_heap()
{
    LOG_WRN("Out of heap");
    std::terminate();
}

int main()
{
    print_application_intro();
    std::set_new_handler(out_of_heap);
    main_loop();
    LOG_INF("program terminating...");
    return 0;
}

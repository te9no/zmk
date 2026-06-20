/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_battery_state_changed);

ZMK_EVENT_IMPL(zmk_peripheral_battery_state_changed);

ZMK_EVENT_IMPL(zmk_central_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

ZMK_RELAY_EVENT_HANDLE(zmk_central_battery_state_changed, cb, );
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_central_battery_state_changed, cb, );

#endif

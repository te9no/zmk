/* SPDX-License-Identifier: MIT */
#pragma once
#if IS_ENABLED(CONFIG_ZMK_BT_RECOVERY)
#include <zmk_bt_recovery.h>
#include <zmk/activity.h>
#define BT_RECOVERY(call) call
#else
#define BT_RECOVERY(call) do { } while (0)
#endif

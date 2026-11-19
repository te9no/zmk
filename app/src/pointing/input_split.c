/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_split

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

struct zis_entry {
    uint8_t reg;
    const struct device *dev;
};

#define ZIS_ENTRY(n) {.reg = DT_INST_REG_ADDR(n), .dev = DEVICE_DT_GET(DT_DRV_INST(n))},

static const struct zis_entry proxy_inputs[] = {DT_INST_FOREACH_STATUS_OKAY(ZIS_ENTRY)};

struct tracked_key {
    uint16_t code;
    bool release_pending;
};

static struct tracked_key keys[ARRAY_SIZE(proxy_inputs)][CONFIG_ZMK_INPUT_SPLIT_MAX_TRACKED_KEYS];

#ifdef CONFIG_INPUT_MODE_THREAD
/* Serialize nonblocking queue insertion with its ledger update, including IRQ producers. */
static struct k_spinlock keys_lock;
#define LOCK_KEYS() k_spinlock_key_t lock_key = k_spin_lock(&keys_lock)
#define UNLOCK_KEYS() k_spin_unlock(&keys_lock, lock_key)
#else
/* Synchronous delivery calls listeners inline and has no full queue. */
#define LOCK_KEYS()
#define UNLOCK_KEYS()
#endif

static void retry_releases(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(release_work, retry_releases);

static int release_key(size_t i, struct tracked_key *key, bool sync) {
    int ret = input_report(proxy_inputs[i].dev, INPUT_EV_KEY, key->code, 0, sync, K_NO_WAIT);
    if (ret == 0) {
        *key = (struct tracked_key){0};
    } else {
        key->release_pending = true;
        k_work_schedule(&release_work, K_MSEC(10));
    }
    return ret;
}

static void retry_releases(struct k_work *work) {
    for (size_t i = 0; i < ARRAY_SIZE(proxy_inputs); i++) {
        for (size_t k = 0; k < ARRAY_SIZE(keys[i]); k++) {
            LOCK_KEYS();
            if (keys[i][k].release_pending) {
                release_key(i, &keys[i][k], true);
            }
            UNLOCK_KEYS();
        }
    }
}

static int report_key(size_t i, uint16_t code, int32_t value, bool sync) {
    struct tracked_key *slot = NULL;
    for (size_t k = 0; k < ARRAY_SIZE(keys[i]); k++) {
        if (keys[i][k].code == code) {
            slot = &keys[i][k];
            break;
        }
        if (!slot && !keys[i][k].code) {
            slot = &keys[i][k];
        }
    }
    if (!value && slot && slot->code) {
        return release_key(i, slot, sync);
    }
    if (value && slot && slot->release_pending) {
        /* Deliver the previous up before the next down whenever capacity permits. */
        if (release_key(i, slot, true) < 0) {
            /* The consumer still holds this key; it must stay down for the new press. */
            slot->release_pending = false;
            return 0;
        }
    }
    int ret = input_report(proxy_inputs[i].dev, INPUT_EV_KEY, code, value, sync, K_NO_WAIT);
    if (!ret && value && slot) {
        slot->code = code;
    }
    return ret;
}

int zmk_input_split_report_peripheral_event(uint8_t reg, uint8_t type, uint16_t code, int32_t value,
                                            bool sync) {
    for (size_t i = 0; i < ARRAY_SIZE(proxy_inputs); i++) {
        if (reg == proxy_inputs[i].reg) {
            if (type != INPUT_EV_KEY || !code) {
                return input_report(proxy_inputs[i].dev, type, code, value, sync, K_NO_WAIT);
            }
            LOCK_KEYS();
            int ret = report_key(i, code, value, sync);
            UNLOCK_KEYS();
            return ret;
        }
    }
    return -ENODEV;
}

int zmk_input_split_peripheral_disconnected(uint8_t reg) {
    for (size_t i = 0; i < ARRAY_SIZE(proxy_inputs); i++) {
        if (reg != proxy_inputs[i].reg) {
            continue;
        }
        LOCK_KEYS();
        for (size_t k = 0; k < ARRAY_SIZE(keys[i]); k++) {
            if (keys[i][k].code) {
                release_key(i, &keys[i][k], true);
            }
        }
        UNLOCK_KEYS();
        return 0;
    }
    return -ENODEV;
}

#define ZIS_INST(n)                                                                                \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_ZMK_INPUT_SPLIT_INIT_PRIORITY, NULL);

#else

#include <zmk/split/peripheral.h>

#define ZIS_INST(n)                                                                                \
    static const struct zmk_input_processor_entry processors_##n[] =                               \
        COND_CODE_1(DT_INST_NODE_HAS_PROP(n, input_processors),                                    \
                    ({LISTIFY(DT_INST_PROP_LEN(n, input_processors),                               \
                              ZMK_INPUT_PROCESSOR_ENTRY_AT_IDX, (, ), DT_DRV_INST(n))}),           \
                    ({}));                                                                         \
    BUILD_ASSERT(DT_INST_NODE_HAS_PROP(n, device),                                                 \
                 "Peripheral input splits need an `input` property set");                          \
    void split_input_handler_##n(struct input_event *evt, void *user_data) {                       \
        for (size_t i = 0; i < ARRAY_SIZE(processors_##n); i++) {                                  \
            int ret = zmk_input_processor_handle_event(processors_##n[i].dev, evt,                 \
                                                       processors_##n[i].param1,                   \
                                                       processors_##n[i].param2, NULL);            \
            if (ret != ZMK_INPUT_PROC_CONTINUE) {                                                  \
                return;                                                                            \
            }                                                                                      \
        }                                                                                          \
        struct zmk_split_transport_peripheral_event ev = {                                         \
            .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,                         \
            .data = {.input_event = {                                                              \
                         .reg = DT_INST_REG_ADDR(n),                                               \
                         .type = evt->type,                                                        \
                         .code = evt->code,                                                        \
                         .value = evt->value,                                                      \
                         .sync = evt->sync,                                                        \
                     }}};                                                                          \
        zmk_split_peripheral_report_event(&ev);                                                    \
    }                                                                                              \
    INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, device)), split_input_handler_##n, NULL);

#endif

DT_INST_FOREACH_STATUS_OKAY(ZIS_INST)

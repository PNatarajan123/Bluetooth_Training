#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/lbs.h>
#include <zephyr/bluetooth/services/dis.h>

#include <zephyr/settings/settings.h>

#include <dk_buttons_and_leds.h>

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)

#define RUN_STATUS_LED          DK_LED1
#define CON_STATUS_LED          DK_LED2
#define RUN_LED_BLINK_INTERVAL  1000

#define USER_LED                DK_LED3

#define USER_BUTTON             DK_BTN1_MSK
#define TOGGLE_BUTTON           DK_BTN2_MSK
#define DISCONNECT_BUTTON       DK_BTN3_MSK
#define CHANGE_NAME_BUTTON      DK_BTN4_MSK

static bool app_button_state;
static bool is_advertising = false;
static struct bt_conn *current_conn = NULL;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LBS_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    printk("Connected\n");
    current_conn = bt_conn_ref(conn);

    dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %u)\n", reason);

    dk_set_led_off(CON_STATUS_LED);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        printk("Security changed: %s level %u\n", addr, level);
    } else {
        printk("Security failed: %s level %u err %d\n", addr, level, err);
    }
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected        = connected,
    .disconnected     = disconnected,
#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
    .security_changed = security_changed,
#endif
};

#if defined(CONFIG_BT_LBS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing cancelled: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .passkey_display = auth_passkey_display,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif

static void app_led_cb(bool led_state)
{
    dk_set_led(USER_LED, led_state);
}

static bool app_button_cb(void)
{
    return app_button_state;
}

static struct bt_lbs_cb lbs_callbacs = {
    .led_cb    = app_led_cb,
    .button_cb = app_button_cb,
};

static void button_changed(uint32_t button_state, uint32_t has_changed)
{
    if (has_changed & USER_BUTTON) {
        uint32_t user_button_state = button_state & USER_BUTTON;

        bt_lbs_send_button_state(user_button_state);
        app_button_state = user_button_state ? true : false;
    }

    if (has_changed & TOGGLE_BUTTON) {
        if (is_advertising) {
            int err = bt_le_adv_stop(); // stop advertising
            if (err) {
                printk("Failed to stop advertising (err %d)\n", err);
            } else {
                printk("Advertising stopped\n");
                is_advertising = false;
            }
        } else {
            int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd)); // start advertising
            if (err) {
                printk("Failed to start advertising (err %d)\n", err);
            } else {
                printk("Advertising started\n");
                is_advertising = true;
            }
        }
    }

    if (has_changed & DISCONNECT_BUTTON) {
        if (current_conn) {
            bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }

    // if (has_changed & CHANGE_NAME_BUTTON){
    //     if (is_advertising) {
    //         int err = bt_le_adv_stop(); // stop advertising
    //         if (err) {
    //             printk("Failed to stop advertising (err %d)\n", err);
    //         } else {
    //             printk("Advertising stopped\n");
    //             is_advertising = false;
    //         }
    //     } else{
    //         static const struct bt_data newad[] = {
    //             BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    //             BT_DATA(BT_DATA_NAME_COMPLETE, "Changed_Name", 11),
    //         };

    //         int err = bt_le_adv_start(BT_LE_ADV_CONN, newad, ARRAY_SIZE(newad), sd, ARRAY_SIZE(sd)); // start advertising
    //         if (err) {
    //             printk("Failed to start advertising (err %d)\n", err);
    //         } else {
    //             printk("Advertising started with new name\n");
    //             is_advertising = true;
    //         }
    //     }
    // }
}

static int init_button(void)
{
    int err;

    err = dk_buttons_init(button_changed);
    if (err) {
        printk("Cannot init buttons (err: %d)\n", err);
    }

    return err;
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static int settings_runtime_load(void)
{
#if defined(CONFIG_BT_GATT_DIS_SETTINGS)
#endif
	return 0;
}


int main(void)
{
    int blink_status = 0;
    int err;

    printk("Starting Bluetooth Peripheral LBS example\n");

    err = dk_leds_init();
    if (err) {
        printk("LEDs init failed (err %d)\n", err);
        return 0;
    }

    err = init_button();
    if (err) {
        printk("Button init failed (err %d)\n", err);
        return 0;
    }

    if (IS_ENABLED(CONFIG_BT_LBS_SECURITY_ENABLED)) {
        err = bt_conn_auth_cb_register(&conn_auth_callbacks);
        if (err) {
            printk("Failed to register authorization callbacks.\n");
            return 0;
        }

        err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
        if (err) {
            printk("Failed to register authorization info callbacks.\n");
            return 0;
        }
    }

    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }

    printk("Bluetooth initialized\n");

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    err = settings_runtime_load(); //device information service
    if(err){
        printk("Device Service Failed to start");
        return 0;
    }

    err = bt_lbs_init(&lbs_callbacs);
    if (err) {
        printk("Failed to init LBS (err:%d)\n", err);
        return 0;
    }

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
                          sd, ARRAY_SIZE(sd));
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return 0;
    } else {
        is_advertising = true;
    }

    printk("Advertising successfully started\n");

    for (;;) {
        dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
        k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
    }
}

/** @file
 *  @brief GATT Device Information Service
 */

/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_BLUETOOTH_SERVICES_DIS_H_
#define ZEPHYR_INCLUDE_BLUETOOTH_SERVICES_DIS_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_SERVICES_DIS_H_ */

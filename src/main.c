
/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <usb/usb_device.h>
#include <usb/class/usb_hid.h>

#define LOG_LEVEL LOG_LEVEL_DBG
LOG_MODULE_REGISTER(main);

#define BT_UUID_UART                                                           \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393,     \
					       0xE0A9, 0xE50E24DCCA9E))
#define BT_UUID_UART_RX                                                        \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393,     \
					       0xE0A9, 0xE50E24DCCA9E))

static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

static volatile uint8_t status[4];
static K_SEM_DEFINE(usb_sem, 1, 1); /* starts off "available" */
static enum usb_dc_status_code usb_status;
K_MSGQ_DEFINE(uart_msgq, 8, 16, 4);

static void in_ready_cb(const struct device *dev)
{
	k_sem_give(&usb_sem);
}

static const struct hid_ops ops = {
	.int_in_ready = in_ready_cb,
};

static void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	usb_status = status;
}

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err 0x%02x)\n", err);
	} else {
		printk("Connected\n");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason 0x%02x)\n", reason);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static void bt_ready(void)
{
	int err;

	printk("Bluetooth initialized\n");

	err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.cancel = auth_cancel,
};

static ssize_t write_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf_, uint16_t len, uint16_t offset,
			uint8_t flags)
{
	const uint8_t *buf = buf_;
	int err;

	if (IS_ENABLED(CONFIG_USB_DEVICE_REMOTE_WAKEUP)) {
		if (usb_status == USB_DC_SUSPEND) {
			LOG_DBG("usb was suspended. ignore write");
			usb_wakeup_request();
			return 0;
		}
	}

	if (offset != 0 || len != 8) {
		LOG_WRN("got invalid write of length %u to offset %zu", len,
			offset);
		return -EPERM;
	}

	err = k_msgq_put(&uart_msgq, buf, K_NO_WAIT);
	if (err) {
		LOG_ERR("failed to queue: %d", err);
		return -ENOMEM;
	}
	LOG_DBG("queued");

	return len;
}

BT_GATT_SERVICE_DEFINE(
	gatt_uart, BT_GATT_PRIMARY_SERVICE(BT_UUID_UART),
	BT_GATT_CHARACTERISTIC(BT_UUID_UART_RX,
			       BT_GATT_CHRC_WRITE |
				       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_rx, NULL));

void main(void)
{
	int err;
	const struct device *hid_dev;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	bt_ready();

	bt_conn_cb_register(&conn_callbacks);
	bt_conn_auth_cb_register(&auth_cb_display);

	hid_dev = device_get_binding("HID_0");
	if (hid_dev == NULL) {
		LOG_ERR("Cannot get USB HID Device");
		return;
	}

	usb_hid_register_device(hid_dev, hid_report_desc,
				sizeof(hid_report_desc), &ops);

	usb_hid_init(hid_dev);

	err = usb_enable(status_cb);
	if (err) {
		LOG_ERR("Failed to enable USB (err %d)", err);
		return;
	}

	for (;;) {
		uint8_t rep[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};

		err = k_msgq_get(&uart_msgq, rep, K_FOREVER);
		if (err) {
			continue;
		}

		k_sem_take(&usb_sem, K_FOREVER);
		hid_int_ep_write(hid_dev, rep, sizeof(rep), NULL);
		LOG_DBG("wrote event");

		k_sem_take(&usb_sem, K_FOREVER);
		memset(rep, 0, sizeof(rep));
		hid_int_ep_write(hid_dev, rep, sizeof(rep), NULL);
		LOG_DBG("cleared");
	}
}

/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 Full-Node Sample
 * ==========================
 * Combines three sensor sources into one BThome V2 BLE advertisement:
 *
 *  1. Die temperature   — nRF52840 internal temperature sensor (always present)
 *  2. 6-axis IMU        — MPU-6050 (nRF52840-DK) or LSM6DS3/LSM6DSL (XIAO Sense)
 *                         Advertises acceleration and gyroscope magnitude.
 *  3. PIR motion sensor — GPIO binary sensor (active-high, DT alias "pir-sensor")
 *
 * The device advertises a new BThome V2 packet every ADVERT_INTERVAL_SEC seconds.
 *
 * Build (nRF52840-DK with MPU-6050 breakout):
 *   west build -b nrf52840dk/nrf52840 samples/bthome_full_node
 *
 * Build (Seeed XIAO nRF52840 Sense):
 *   west build -b seeed_xiao_ble/nrf52840 samples/bthome_full_node
 *
 * Verify with bthome-logger:
 *   uv tool install bthome-logger && bthome-logger -f "MAKE"
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>

#include <bthome_v2/bthome_v2.h>

#include "sensors/sensor_die_temp.h"
#include "sensors/sensor_imu.h"
#include "sensors/sensor_pir.h"

LOG_MODULE_REGISTER(bthome_node, LOG_LEVEL_INF);

/* ── Timing ──────────────────────────────────────────────────────────────── */
#define ADVERT_INTERVAL_SEC  10

/* ── BLE advertising parameters ─────────────────────────────────────────── */
static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     BT_GAP_ADV_SLOW_INT_MIN,
			     BT_GAP_ADV_SLOW_INT_MAX,
			     NULL);

/* ── BThome V2 context ───────────────────────────────────────────────────── */
static struct bthome_v2_ctx bthome;

/* ── Advertising data: Flags + Name + BThome service data ──────────────── */
/* Layout: flags(3B) + name(10B) + svcdata(18B) = 31B exactly — no overflow */
static struct bt_data ad[3];

/* ── Packet counter (incremented each advertisement) ────────────────────── */
static uint8_t pkt_id;

/* ── LED2 (alias "led1" on nRF52840-DK) ─────────────────────────────────── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay)
static const struct gpio_dt_spec led2 =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#define HAS_LED2 1
#else
#define HAS_LED2 0
#endif

/* ── Sensor read & advertise ─────────────────────────────────────────────── */

static void update_advertisement(void)
{
	int ret;

#if HAS_LED2
	gpio_pin_set_dt(&led2, 1);
#endif

	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id++);

	sensor_die_temp_update(&bthome);
	sensor_imu_update(&bthome);
	sensor_pir_update(&bthome);

	/* ── Encode & update advertising ─────────────────────────────────── */
	ret = bthome_v2_encode(&bthome);
	if (ret < 0) {
		LOG_ERR("BThome encode failed: %d", ret);
		return;
	}

	ret = bthome_v2_get_bt_data(&bthome, &ad[2]);
	if (ret < 0) {
		LOG_ERR("bthome_v2_get_bt_data failed: %d", ret);
		return;
	}

	ret = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret < 0) {
		LOG_ERR("BLE adv update failed: %d", ret);
	}

#if HAS_LED2
	k_sleep(K_MSEC(50));
	gpio_pin_set_dt(&led2, 0);
#endif
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
	int ret;

	LOG_INF("BThome V2 Full-Node starting");

	/* ── Initialise LED2 ────────────────────────────────────────────── */
#if HAS_LED2
	if (gpio_is_ready_dt(&led2)) {
		gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
	}
#endif

	/* ── Initialise sensors ─────────────────────────────────────────── */
	sensor_pir_init();

	/* ── Initialise BThome V2 context ───────────────────────────────── */
	bthome_v2_init(&bthome, false, false);

	/* ── Build initial advertising data ────────────────────────────── */
	ad[0] = (struct bt_data)
		BT_DATA_BYTES(BT_DATA_FLAGS,
			      BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);
	ad[1] = (struct bt_data)
		BT_DATA(BT_DATA_NAME_COMPLETE,
			CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1);

	bthome_v2_add_packet_id(&bthome, 0);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[2]);
	bthome_v2_clear(&bthome);

	/* ── Enable Bluetooth ───────────────────────────────────────────── */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("bt_enable failed: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth enabled");

	/* ── Start advertising ──────────────────────────────────────────── */
	ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("bt_le_adv_start failed: %d", ret);
		return ret;
	}
	LOG_INF("Advertising started (interval ~%d ms)",
		BT_GAP_ADV_SLOW_INT_MIN * 625 / 1000);

	/* ── Main loop ──────────────────────────────────────────────────── */
	while (true) {
		update_advertisement();
		k_sleep(K_SECONDS(ADVERT_INTERVAL_SEC));
	}

	return 0;
}

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
 *   uv tool install bthome-logger && bthome-logger -f "BThome-Sensor"
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#include <bthome_v2/bthome_v2.h>

LOG_MODULE_REGISTER(bthome_node, LOG_LEVEL_INF);

/* ── Timing ──────────────────────────────────────────────────────────────── */
#define ADVERT_INTERVAL_SEC  10

/* ── BLE advertising parameters (non-connectable, undirected) ────────────── */
static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     BT_GAP_ADV_SLOW_INT_MIN,
			     BT_GAP_ADV_SLOW_INT_MAX,
			     NULL);

/* ── BThome V2 context ───────────────────────────────────────────────────── */
static struct bthome_v2_ctx bthome;

/* ── Advertising data array (flags + service data) ───────────────────────── */
static struct bt_data ad[2];

/* ── Packet counter (incremented each advertisement) ────────────────────── */
static uint8_t pkt_id;

/* ── Devices ─────────────────────────────────────────────────────────────── */

/* Internal die temperature sensor (always present on nRF52840) */
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_temp)
static const struct device *die_temp_dev =
	DEVICE_DT_GET_ONE(nordic_nrf_temp);
#else
static const struct device *die_temp_dev;
#endif

/* IMU — resolved via DT alias "imu" set in board overlays */
#if DT_NODE_HAS_STATUS(DT_ALIAS(imu), okay)
static const struct device *imu_dev =
	DEVICE_DT_GET(DT_ALIAS(imu));
#define HAS_IMU 1
#else
static const struct device *imu_dev;
#define HAS_IMU 0
#endif

/* PIR sensor GPIO — resolved via DT alias "pir-sensor".
 * Note: DT_ALIAS() converts underscores to hyphens per Zephyr convention,
 * so DT_ALIAS(pir_sensor) correctly resolves the "pir-sensor" alias defined
 * in the board overlay files.
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(pir_sensor), okay)
static const struct gpio_dt_spec pir =
	GPIO_DT_SPEC_GET(DT_ALIAS(pir_sensor), gpios);
#define HAS_PIR 1
#else
#define HAS_PIR 0
#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/**
 * @brief Convert Zephyr sensor_value to BThome temperature (0.01 °C).
 *
 * sensor_value carries the integer part in .val1 and micro-fractions in .val2.
 * For temperature, .val2 is in µ°C (1/1 000 000 °C).
 * BThome 0x02 uses 0.01 °C → multiply val1 by 100, val2 by 100/1 000 000.
 */
static int16_t sv_to_temp_cdegc(const struct sensor_value *sv)
{
	return (int16_t)(sv->val1 * 100 + sv->val2 / 10000);
}

/**
 * @brief Convert Zephyr sensor_value acceleration (m/s²) to BThome unit
 *        (0.001 m/s² → store as mm/s²).
 */
static uint16_t sv_to_accel_mms2(const struct sensor_value *sv)
{
	int32_t mm = sv->val1 * 1000 + sv->val2 / 1000;

	return (uint16_t)(mm < 0 ? -mm : mm);
}

/**
 * @brief Convert Zephyr sensor_value gyroscope (rad/s) to BThome unit
 *        (0.001 °/s).
 *
 * Zephyr gyro channels use rad/s.  BThome 0x52 uses 0.001 °/s.
 * 1 rad/s ≈ 57295 m°/s → scale val1 and val2 accordingly.
 */
static uint16_t sv_to_gyro_mdegps(const struct sensor_value *sv)
{
	/* Convert rad/s → m°/s: multiply by 57295 (= 1000 * 180/π) */
	int32_t mdeg = sv->val1 * 57295 + (int32_t)(sv->val2 / 1000) * 57 / 1000;

	return (uint16_t)(mdeg < 0 ? -mdeg : mdeg);
}

/* ── Sensor read & advertise ─────────────────────────────────────────────── */

static void update_advertisement(void)
{
	int ret;

	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id++);

	/* ── 1. Die temperature ─────────────────────────────────────────── */
	if (die_temp_dev && device_is_ready(die_temp_dev)) {
		struct sensor_value temp_sv;

		ret = sensor_sample_fetch(die_temp_dev);
		if (ret == 0) {
			ret = sensor_channel_get(die_temp_dev,
						 SENSOR_CHAN_DIE_TEMP,
						 &temp_sv);
		}
		if (ret == 0) {
			int16_t temp = sv_to_temp_cdegc(&temp_sv);

			bthome_v2_add_temperature(&bthome, temp);
			LOG_INF("Die temp: %d.%02d °C",
				temp / 100, (int)(temp < 0 ? -temp : temp) % 100);
		} else {
			LOG_WRN("Die temp read failed: %d", ret);
		}
	}

	/* ── 2. IMU ─────────────────────────────────────────────────────── */
#if HAS_IMU
	if (imu_dev && device_is_ready(imu_dev)) {
		struct sensor_value accel[3];
		struct sensor_value gyro[3];

		ret = sensor_sample_fetch(imu_dev);
		if (ret == 0) {
			ret = sensor_channel_get(imu_dev,
						 SENSOR_CHAN_ACCEL_XYZ, accel);
		}
		if (ret == 0) {
			ret = sensor_channel_get(imu_dev,
						 SENSOR_CHAN_GYRO_XYZ, gyro);
		}

		if (ret == 0) {
			/* Advertise X-axis acceleration and Z-axis gyro as
			 * representative single-axis values.  Home Assistant
			 * will show them as individual sensor entities. */
			uint16_t ax = sv_to_accel_mms2(&accel[0]);
			uint16_t gz = sv_to_gyro_mdegps(&gyro[2]);

			bthome_v2_add_acceleration(&bthome, ax);
			bthome_v2_add_gyroscope(&bthome, gz);

			LOG_INF("IMU accel_x: %u mm/s²  gyro_z: %u m°/s",
				ax, gz);
		} else {
			LOG_WRN("IMU read failed: %d", ret);
		}
	}
#endif /* HAS_IMU */

	/* ── 3. PIR ─────────────────────────────────────────────────────── */
#if HAS_PIR
	if (gpio_is_ready_dt(&pir)) {
		int pir_state = gpio_pin_get_dt(&pir);

		if (pir_state >= 0) {
			bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION,
					     pir_state != 0);
			LOG_INF("PIR: %s", pir_state ? "DETECTED" : "clear");
		} else {
			LOG_WRN("PIR read failed: %d", pir_state);
		}
	}
#endif /* HAS_PIR */

	/* ── Encode & update advertising ─────────────────────────────────── */
	ret = bthome_v2_encode(&bthome);
	if (ret < 0) {
		LOG_ERR("BThome encode failed: %d", ret);
		return;
	}

	ret = bthome_v2_get_bt_data(&bthome, &ad[1]);
	if (ret < 0) {
		LOG_ERR("bthome_v2_get_bt_data failed: %d", ret);
		return;
	}

	ret = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret < 0) {
		LOG_ERR("BLE adv update failed: %d", ret);
	}
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
	int ret;

	LOG_INF("BThome V2 Full-Node starting");

	/* ── Initialise GPIO (PIR) ──────────────────────────────────────── */
#if HAS_PIR
	if (!gpio_is_ready_dt(&pir)) {
		LOG_ERR("PIR GPIO device not ready");
	} else {
		ret = gpio_pin_configure_dt(&pir, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("PIR GPIO configure failed: %d", ret);
		}
	}
#endif

	/* ── Initialise BThome V2 context ───────────────────────────────── */
	bthome_v2_init(&bthome, false, false);

	/* ── Build initial advertising data (flags AD) ──────────────────── */
	ad[0] = (struct bt_data)
		BT_DATA_BYTES(BT_DATA_FLAGS,
			      BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);

	/* Populate ad[1] with an initial encode so we have valid data
	 * before calling bt_le_adv_start().                            */
	bthome_v2_add_packet_id(&bthome, 0);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[1]);
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

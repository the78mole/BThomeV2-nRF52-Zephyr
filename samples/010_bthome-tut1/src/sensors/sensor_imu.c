/* SPDX-License-Identifier: MIT */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include "sensor_imu.h"

LOG_MODULE_DECLARE(bthome_node, LOG_LEVEL_INF);

#if DT_NODE_HAS_STATUS(DT_ALIAS(imu), okay)
static const struct device *imu_dev = DEVICE_DT_GET(DT_ALIAS(imu));
#define HAS_IMU 1
#else
static const struct device *imu_dev;
#define HAS_IMU 0
#endif

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
	int32_t mdeg = sv->val1 * 57295 + (int32_t)(sv->val2 / 1000) * 57 / 1000;

	return (uint16_t)(mdeg < 0 ? -mdeg : mdeg);
}

void sensor_imu_update(struct bthome_v2_ctx *ctx)
{
#if HAS_IMU
	if (!imu_dev || !device_is_ready(imu_dev)) {
		return;
	}

	struct sensor_value accel[3];
	struct sensor_value gyro[3];
	int ret = sensor_sample_fetch(imu_dev);

	if (ret == 0) {
		ret = sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
	}
	if (ret == 0) {
		ret = sensor_channel_get(imu_dev, SENSOR_CHAN_GYRO_XYZ, gyro);
	}
	if (ret < 0) {
		LOG_WRN("IMU read failed: %d", ret);
		return;
	}

	uint16_t ax = sv_to_accel_mms2(&accel[0]);
	uint16_t gz = sv_to_gyro_mdegps(&gyro[2]);

	bthome_v2_add_acceleration(ctx, ax);
	bthome_v2_add_gyroscope(ctx, gz);
	LOG_INF("IMU accel_x: %u mm/s²  gyro_z: %u m°/s", ax, gz);
#else
	ARG_UNUSED(ctx);
#endif /* HAS_IMU */
}

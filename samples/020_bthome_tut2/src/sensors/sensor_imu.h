/* SPDX-License-Identifier: MIT */
#ifndef SENSOR_IMU_H
#define SENSOR_IMU_H

#include <bthome_v2/bthome_v2.h>

/**
 * @brief Read IMU (acceleration + gyroscope) and add values to the BThome context.
 *
 * Requires DT alias "imu" to be defined and enabled in the board overlay.
 * No-op if the IMU device is not available.
 */
void sensor_imu_update(struct bthome_v2_ctx *ctx);

#endif /* SENSOR_IMU_H */

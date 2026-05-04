/* SPDX-License-Identifier: MIT */
#ifndef SENSOR_DIE_TEMP_H
#define SENSOR_DIE_TEMP_H

#include <bthome_v2/bthome_v2.h>

/**
 * @brief Read die temperature and add it to the BThome context.
 *
 * No-op if the nordic_nrf_temp device is not available.
 */
void sensor_die_temp_update(struct bthome_v2_ctx *ctx);

#endif /* SENSOR_DIE_TEMP_H */

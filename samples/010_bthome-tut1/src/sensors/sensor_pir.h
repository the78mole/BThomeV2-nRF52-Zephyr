/* SPDX-License-Identifier: MIT */
#ifndef SENSOR_PIR_H
#define SENSOR_PIR_H

#include <bthome_v2/bthome_v2.h>

/**
 * @brief Configure the PIR GPIO pin.
 *
 * Requires DT alias "pir-sensor" to be defined and enabled in the board overlay.
 * No-op if the alias is absent.
 */
void sensor_pir_init(void);

/**
 * @brief Read PIR state and add motion object to the BThome context.
 *
 * No-op if the PIR alias is absent.
 */
void sensor_pir_update(struct bthome_v2_ctx *ctx);

#endif /* SENSOR_PIR_H */

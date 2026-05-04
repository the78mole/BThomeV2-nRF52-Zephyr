/* SPDX-License-Identifier: MIT */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include "sensor_die_temp.h"

LOG_MODULE_DECLARE(bthome_node, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_temp)
static const struct device *die_temp_dev =
	DEVICE_DT_GET_ONE(nordic_nrf_temp);
#else
static const struct device *die_temp_dev;
#endif

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

void sensor_die_temp_update(struct bthome_v2_ctx *ctx)
{
	if (!die_temp_dev || !device_is_ready(die_temp_dev)) {
		return;
	}

	struct sensor_value temp_sv;
	int ret = sensor_sample_fetch(die_temp_dev);

	if (ret == 0) {
		ret = sensor_channel_get(die_temp_dev, SENSOR_CHAN_DIE_TEMP, &temp_sv);
	}
	if (ret < 0) {
		LOG_WRN("Die temp read failed: %d", ret);
		return;
	}

	int16_t temp = sv_to_temp_cdegc(&temp_sv);

	bthome_v2_add_temperature(ctx, temp);
	LOG_INF("Die temp: %d.%02d °C", temp / 100, (int)(temp < 0 ? -temp : temp) % 100);
}

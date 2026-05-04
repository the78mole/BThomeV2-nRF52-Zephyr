/* SPDX-License-Identifier: MIT */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include "sensor_pir.h"

LOG_MODULE_DECLARE(bthome_node, LOG_LEVEL_INF);

/* Note: DT_ALIAS() converts hyphens to underscores per Zephyr convention,
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

void sensor_pir_init(void)
{
#if HAS_PIR
	if (!gpio_is_ready_dt(&pir)) {
		LOG_ERR("PIR GPIO device not ready");
		return;
	}

	int ret = gpio_pin_configure_dt(&pir, GPIO_INPUT);

	if (ret < 0) {
		LOG_ERR("PIR GPIO configure failed: %d", ret);
	}
#endif
}

void sensor_pir_update(struct bthome_v2_ctx *ctx)
{
#if HAS_PIR
	if (!gpio_is_ready_dt(&pir)) {
		return;
	}

	int pir_state = gpio_pin_get_dt(&pir);

	if (pir_state < 0) {
		LOG_WRN("PIR read failed: %d", pir_state);
		return;
	}

	bthome_v2_add_binary(ctx, BTHOME_OBJ_MOTION, pir_state != 0);
	LOG_INF("PIR: %s", pir_state ? "DETECTED" : "clear");
#else
	ARG_UNUSED(ctx);
#endif /* HAS_PIR */
}

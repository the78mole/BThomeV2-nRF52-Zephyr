/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * Blinky sample for nRF52840-DK and Seeed XIAO nRF52840 (Sense & Plus)
 *
 * LED timing: 100 ms ON / 900 ms OFF
 *
 * Build:
 *   west build -b nrf52840dk/nrf52840    samples/blinky
 *   west build -b seeed_xiao_ble/nrf52840 samples/blinky
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* Use the board-agnostic "led0" alias defined in every target's DTS. */
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	while (true) {
		/* 100 ms ON */
		gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(100));

		/* 900 ms OFF */
		gpio_pin_set_dt(&led, 0);
		k_sleep(K_MSEC(900));
	}

	return 0;
}

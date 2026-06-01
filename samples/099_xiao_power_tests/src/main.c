/* SPDX-License-Identifier: MIT */
/*
 * XIAO nRF52840 (Sense) — Power Baseline Test
 * ============================================
 * Does nothing.  Boots, drops the BLE Tx indicator LOW, and parks the CPU
 * in k_sleep(K_FOREVER).  The Zephyr idle thread issues WFI; with all
 * peripherals disabled in the overlay this should reach the lowest System
 * ON / CPU asleep floor the board can deliver (~3 µA on a bare nRF52840).
 *
 * Used to isolate the effect of individual XIAO Sense peripherals (mic,
 * IMU, QSPI flash, BQ25101 charger) by toggling overlay and Kconfig
 * options one at a time and re-measuring with the PPK2.
 *
 * D5 (P0.05) is driven HIGH during boot, LOW before WFI — serves as a
 * boot-done marker for the PPK2 logic analyser channel.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#ifdef CONFIG_APP_SYSTEM_OFF
#include <zephyr/sys/poweroff.h>
#include <hal/nrf_gpio.h>

/* ── P25Q16H QSPI pin assignments on XIAO BLE Sense (from xiao_ble-pinctrl.dtsi) ──
 *   P0.20 = QSPI_IO0 (MOSI in SPI mode)
 *   P0.21 = QSPI_SCK
 *   P0.25 = QSPI_CSN (active-LOW)
 * Flash uses SPI mode 0 (CPOL=0, CPHA=0), MSB first.
 */
#define FLASH_PIN_MOSI  20u
#define FLASH_PIN_SCK   21u
#define FLASH_PIN_CSN   25u
#define FLASH_CMD_DPD   0xB9u   /* Deep Power-Down */

static void flash_deep_power_down(void)
{
	/* Configure SPI pins as outputs */
	nrf_gpio_cfg_output(FLASH_PIN_CSN);
	nrf_gpio_cfg_output(FLASH_PIN_SCK);
	nrf_gpio_cfg_output(FLASH_PIN_MOSI);

	/* Idle state: CSN high, SCK low */
	nrf_gpio_pin_set(FLASH_PIN_CSN);
	nrf_gpio_pin_clear(FLASH_PIN_SCK);
	nrf_gpio_pin_clear(FLASH_PIN_MOSI);

	/* Assert chip-select */
	nrf_gpio_pin_clear(FLASH_PIN_CSN);

	/* Clock out 0xB9 MSB-first, SPI mode 0 */
	for (int i = 7; i >= 0; i--) {
		nrf_gpio_pin_write(FLASH_PIN_MOSI, (FLASH_CMD_DPD >> i) & 1u);
		nrf_gpio_pin_set(FLASH_PIN_SCK);   /* rising edge — data sampled */
		nrf_gpio_pin_clear(FLASH_PIN_SCK); /* falling edge */
	}

	/* Deassert chip-select → DPD command complete */
	nrf_gpio_pin_set(FLASH_PIN_CSN);

	/* t_enter_dpd = 3 µs (P25Q16H datasheet) */
	k_busy_wait(10u);
}
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(active_indicator), okay)
static const struct gpio_dt_spec led_tx =
	GPIO_DT_SPEC_GET(DT_ALIAS(active_indicator), gpios);
#define HAS_LED_TX 1
#else
#define HAS_LED_TX 0
#endif

int main(void)
{
#if HAS_LED_TX
	if (gpio_is_ready_dt(&led_tx)) {
		gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_ACTIVE);
		k_sleep(K_MSEC(50));
		gpio_pin_set_dt(&led_tx, 0);
	}
#endif

#ifdef CONFIG_APP_SYSTEM_OFF
	/* Send P25Q16H QSPI flash to Deep Power-Down via direct SPI bit-bang.
	 * The Zephyr nordic_qspi-nor driver is disabled (qspi: disabled in DT)
	 * to avoid HFCLK drain from driver init.  We replicate what the Arduino
	 * Adafruit_SPIFlash library does: send opcode 0xB9 directly, then call
	 * NRF_POWER->SYSTEMOFF.
	 */
	flash_deep_power_down();

	/* Hardware-only floor: kill CPU, RAM (except retained), all peripherals.
	 * Anything still drawing current after this is *physical* (LEDs, BQ25101,
	 * external pull-ups, board leakage).  Wake-up only via reset pin.
	 */
	sys_poweroff();
#else
	k_sleep(K_FOREVER);
#endif
	return 0;
}

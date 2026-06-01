/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 PIR Sample
 * ====================
 * Detects motion via a PIR sensor and advertises via BThome V2 BLE.
 *
 * Power strategy — System ON / CPU asleep (~3–15 µA):
 * ────────────────────────────────────────────────────
 * nRF52840 System OFF (~0.5 µA) is NOT used: nrf_power_system_off() falls
 * into while(true){__WFE()} after writing SYSTEMOFF=1, and MPSL generates
 * DPPI events that immediately wake __WFE() — the chip never actually
 * reaches System OFF.
 *
 * Instead, the same approach as sample 020 is used:
 *   bt_disable() releases the HFCLK held by the BLE controller (~640 µA).
 *   k_event_wait(timeout) parks the CPU in WFI via the Zephyr idle thread.
 *   With HFCLK released and CPU in WFI the nRF52840 draws ~3–15 µA.
 *
 * Two wake sources (both in System ON):
 *   PIR active-level PORT event → sets EVENT_PIR in ISR
 *   k_event_wait timeout             → keepalive advertisement
 *
 * Per-cycle sequence:
 *   k_event_wait(EVENT_PIR, K_MSEC(ADV_SLOW_INTERVAL_MS)) [CPU sleeps here]
 *   bt_enable(NULL)
 *   adv_send(motion, pkt_id++)
 *   k_sleep(burst or confirm window)
 *   bt_le_adv_stop() + bt_disable()                       [releases HFCLK]
 *   → back to k_event_wait
 *
 * Build (nRF52840-DK):
 *   west build -b nrf52840dk/nrf52840 samples/100_bthome_pir
 *
 * Build (Seeed XIAO nRF52840 Sense):
 *   west build -b xiao_ble_sense samples/100_bthome_pir
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

LOG_MODULE_REGISTER(bthome_pir, LOG_LEVEL_INF);

/* ── Timing ──────────────────────────────────────────────────────────────── */
/** Duration in ms for which motion=1 burst advertising is active. */
#define BURST_DURATION_MS       2000U
/** Duration in ms of the short motion=0 confirmation window. */
#define ADV_CONFIRM_MS          150U
/** Keepalive interval: how long the CPU sleeps between advertisements. */
#define ADV_SLOW_INTERVAL_MS    30000U

/* ── BLE advertising parameters ─────────────────────────────────────────── */
#define ADV_FAST_INTERVAL_MS  50U
#define ADV_FAST_INT          (ADV_FAST_INTERVAL_MS * 8U / 5U)  /* BLE units */

static const struct bt_le_adv_param adv_fast =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     ADV_FAST_INT, ADV_FAST_INT, NULL);

/* ── BThome V2 context ───────────────────────────────────────────────────── */
static struct bthome_v2_ctx bthome;
static struct bt_data ad[3];   /* flags, complete name, service data */

/* ── PIR GPIO ────────────────────────────────────────────────────────────── */
static const struct gpio_dt_spec pir =
	GPIO_DT_SPEC_GET(DT_ALIAS(pir_sensor), gpios);

/* ── Active indicator (alias "active-indicator" = D5 = P0.05) ──────────── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(active_indicator), okay)
static const struct gpio_dt_spec led_tx =
	GPIO_DT_SPEC_GET(DT_ALIAS(active_indicator), gpios);
#define HAS_LED_TX 1
#else
#define HAS_LED_TX 0
#endif

/* ── PIR event flag ──────────────────────────────────────────────────────── */
#define EVENT_PIR  BIT(0)
static K_EVENT_DEFINE(pir_evt);

/* ── PIR GPIO callback ───────────────────────────────────────────────────── */
static struct gpio_callback pir_cb_data;

static void pir_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/*
	 * Disable interrupt immediately: PORT event (LEVEL trigger) would
	 * re-fire continuously as long as PIR is HIGH.  Re-armed at the
	 * end of each advertising cycle via gpio_pin_interrupt_configure_dt.
	 */
	gpio_pin_interrupt_configure_dt(&pir, GPIO_INT_DISABLE);
	k_event_set(&pir_evt, EVENT_PIR);
}

/* ── Packet counter ──────────────────────────────────────────────────────── */
static uint8_t pkt_id;

/* ── Core advertising helper ─────────────────────────────────────────────── */
static void adv_send(bool motion)
{
	int err;

	bt_le_adv_stop();

	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id++);
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, motion);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[2]);

	err = bt_le_adv_start(&adv_fast, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("bt_le_adv_start failed: %d", err);
	} else {
		LOG_INF("ADV: motion=%d pkt_id=%u", (int)motion, pkt_id - 1);
	}
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
	int ret;

	LOG_INF("BThome PIR starting");

	/* ── GPIO init ──────────────────────────────────────────────────── */
	if (!gpio_is_ready_dt(&pir)) {
		LOG_ERR("PIR GPIO not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&pir, GPIO_INPUT);

	/* DIAGNOSTIC: no GPIO interrupt, plain input only. */

#if HAS_LED_TX
	if (gpio_is_ready_dt(&led_tx)) {
		gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE);
	}
#endif

	/* ── BThome context + static ad[] headers ───────────────────────── */
	bthome_v2_init(&bthome, false, false);

	ad[0] = (struct bt_data) BT_DATA_BYTES(BT_DATA_FLAGS,
			BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);
	ad[1] = (struct bt_data) BT_DATA(BT_DATA_NAME_COMPLETE,
			CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1);

	/* ── Enable BT once ─────────────────────────────────────────────── */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("bt_enable failed: %d", ret);
		return ret;
	}

	/* ── Initial keepalive advertisement ────────────────────────────── */
#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 1);
#endif
	adv_send(false);
	k_sleep(K_MSEC(ADV_CONFIRM_MS));
	bt_le_adv_stop();
	bt_disable();
#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 0);
#endif

	/* ── Main loop ──────────────────────────────────────────────────── */
	while (true) {
		/*
		 * DIAGNOSTIC: poll PIR every 250 ms during the sleep window.
		 * Exactly the approach used by sample 020 (which reaches ~10 µA).
		 * If this build also stays at ~1 mA, the issue is not in the
		 * GPIO interrupt path — look elsewhere.
		 */
		bool motion = false;
		for (uint32_t t = 0; t < ADV_SLOW_INTERVAL_MS; t += 250U) {
			k_sleep(K_MSEC(250));
			if (gpio_pin_get_dt(&pir) > 0) {
				motion = true;
				break;
			}
		}

#if HAS_LED_TX
		gpio_pin_set_dt(&led_tx, 1);
#endif

		/* Re-enable BT for this advertising window */
		ret = bt_enable(NULL);
		if (ret) {
			LOG_ERR("bt_enable failed: %d", ret);
			goto sleep_again;
		}

		if (motion) {
			adv_send(true);
			k_sleep(K_MSEC(BURST_DURATION_MS));
			adv_send(false);
		} else {
			adv_send(false);
		}

		k_sleep(K_MSEC(ADV_CONFIRM_MS));
		bt_le_adv_stop();
		bt_disable();  /* releases HFCLK → CPU can enter deep WFI */

sleep_again:
#if HAS_LED_TX
		gpio_pin_set_dt(&led_tx, 0);
#endif
	}

	return 0;
}

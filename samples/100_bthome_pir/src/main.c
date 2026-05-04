/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 PIR Sample
 * ====================
 * Detects motion via a PIR sensor connected to a GPIO (rising-edge interrupt)
 * and advertises the event latency-free over BThome V2 BLE.
 *
 * State machine:
 *   IDLE  → motion detected → BURST advertising (fast interval, motion=1)
 *         → 2 s k_timer expires → SLOW advertising (slow interval, motion=0)
 *
 * Build (nRF52840-DK):
 *   west build -b nrf52840dk/nrf52840 samples/100_bthome_pir
 *
 * Build (Seeed XIAO nRF52840):
 *   west build -b seeed_xiao_ble/nrf52840 samples/100_bthome_pir
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
#define BURST_DURATION_MS   2000
/** Duration in ms for which the TX LED stays on after each sent packet. */
#define LED_TX_ON_MS        50

/* ── BLE advertising parameters ─────────────────────────────────────────── */

/** Fast advertising interval in ms — used for both adv_param and hook delay. */
#define ADV_FAST_INTERVAL_MS   50U
#define ADV_FAST_INT           (ADV_FAST_INTERVAL_MS * 8 / 5)  /* → BLE units */

static const struct bt_le_adv_param adv_fast =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     ADV_FAST_INT,
			     ADV_FAST_INT,
			     NULL);

/** Slow interval for idle state: ~3 s (BLE spec unit: 0.625 ms).
 *  0x0BB8 = 3000 * 1.6 = 4800 units = 3000 ms                              */
#define ADV_SLOW_INTERVAL_MS   3000U
#define ADV_SLOW_INT           (ADV_SLOW_INTERVAL_MS * 8 / 5)   /* → units */

static const struct bt_le_adv_param adv_slow =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     ADV_SLOW_INT,
			     ADV_SLOW_INT,
			     NULL);

/* ── BThome V2 context ───────────────────────────────────────────────────── */
static struct bthome_v2_ctx bthome;
static struct bt_data ad[3];   /* flags, complete name, service data */
static uint8_t pkt_id;

/* ── Extended advertising handles (two sets: fast / slow) ───────────────── */
static struct bt_le_ext_adv *ext_adv_fast_h;
static struct bt_le_ext_adv *ext_adv_slow_h;
static struct bt_le_ext_adv *ext_adv_active; /* currently advertising set */
static bool current_motion;                  /* state for sent-hook re-encode */

/* ── PIR GPIO ────────────────────────────────────────────────────────────── */
static const struct gpio_dt_spec pir =
	GPIO_DT_SPEC_GET(DT_ALIAS(pir_sensor), gpios);

static struct gpio_callback pir_cb_data;

/* ── TX LED (alias "led0") ───────────────────────────────────────────────── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led_tx =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAS_LED_TX 1
#else
#define HAS_LED_TX 0
#endif

/* ── Forward declarations ────────────────────────────────────────────────── */
static void motion_work_handler(struct k_work *work);
static void clear_work_handler(struct k_work *work);
static void update_pkt_work_handler(struct k_work *work);
static void led_off_work_handler(struct k_work *work);
static void burst_timer_handler(struct k_timer *timer);
static void adv_period_timer_handler(struct k_timer *timer);

/* ── Work items and timers ───────────────────────────────────────────────── */
static K_WORK_DEFINE(motion_work, motion_work_handler);
static K_WORK_DEFINE(clear_work, clear_work_handler);
static K_WORK_DEFINE(update_pkt_work, update_pkt_work_handler);
static K_WORK_DELAYABLE_DEFINE(led_off_work, led_off_work_handler);
static K_TIMER_DEFINE(burst_timer, burst_timer_handler, NULL);
static K_TIMER_DEFINE(adv_period_timer, adv_period_timer_handler, NULL);

/* ── Advertising period timer callback (ISR context) ────────────────────── */

static void adv_period_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	/* Hand off to system workqueue — BT API not safe from ISR. */
	k_work_submit(&update_pkt_work);
}

/* ── Work handler: turn TX LED off (system WQ) ───────────────────────────── */

static void led_off_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 0);
#endif
}

/* ── Work handler: increment pkt_id and update payload (system WQ) ─────── */

static void update_pkt_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct bt_le_ext_adv *adv = ext_adv_active;

	if (!adv) {
		return;
	}

#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 1);
	k_work_reschedule(&led_off_work, K_MSEC(LED_TX_ON_MS));
#endif

	bthome_v2_clear(&bthome);
	/* Increment pkt_id only in idle state — during a motion burst the ID
	 * stays constant so the receiver sees it as one continuous event. */
	if (!current_motion) {
		pkt_id++;
	}
	bthome_v2_add_packet_id(&bthome, pkt_id);
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, current_motion);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[2]);

	/* Update payload — controller sends next PDU at the configured interval. */
	int err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("bt_le_ext_adv_set_data failed: %d", err);
	}
}

/* ── BThome advertising helpers ──────────────────────────────────────────── */

/**
 * @brief Switch to a different advertising set on state change.
 *
 * Must be called from thread context (not ISR).
 * pkt_id is incremented on every state change here, so motion=1 and
 * the subsequent motion=0 each get a distinct ID.
 *
 * @param motion   true = motion active, false = no motion.
 * @param new_adv  Extended advertising handle to activate.
 */
static void restart_adv(bool motion, struct bt_le_ext_adv *new_adv,
			uint32_t interval_ms)
{
	int err;

	/* Cancel any pending payload update for the outgoing set. */
	k_work_cancel(&update_pkt_work);

	/* Stop the outgoing set. */
	if (ext_adv_active && ext_adv_active != new_adv) {
		bt_le_ext_adv_stop(ext_adv_active);
	}

	current_motion = motion;
	ext_adv_active  = new_adv;

	/* Encode payload — increment pkt_id on every state change. */
	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id++);
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, motion);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[2]);

	err = bt_le_ext_adv_set_data(new_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("bt_le_ext_adv_set_data failed: %d", err);
		return;
	}

	/* num_events=0: controller advertises continuously at the set interval. */
	struct bt_le_ext_adv_start_param start = { .num_events = 0 };

	err = bt_le_ext_adv_start(new_adv, &start);
	if (err && err != -EALREADY) {
		LOG_ERR("bt_le_ext_adv_start failed: %d", err);
	} else {
		LOG_INF("ADV switched: motion=%d pkt_id=%u", (int)motion, pkt_id);
	}

	/* Re-arm period timer — fires at interval_ms to update pkt_id. */
	k_timer_start(&adv_period_timer,
		      K_MSEC(interval_ms), K_MSEC(interval_ms));
}

/* ── Work handler: motion detected ──────────────────────────────────────── */

static void motion_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Restart burst timer — re-triggers even if already running. */
	k_timer_start(&burst_timer, K_MSEC(BURST_DURATION_MS), K_NO_WAIT);

	restart_adv(true, ext_adv_fast_h, ADV_FAST_INTERVAL_MS);
}

/* ── Work handler: burst expired, switch back to idle ───────────────────── */

static void clear_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	restart_adv(false, ext_adv_slow_h, ADV_SLOW_INTERVAL_MS);
}

/* ── Timer callback (ISR context!) ──────────────────────────────────────── */

static void burst_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	/* Cannot call BLE APIs from ISR → hand off to system workqueue. */
	k_work_submit(&clear_work);
}

/* ── PIR GPIO interrupt callback (ISR context!) ─────────────────────────── */

static void pir_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Schedule motion handling in thread context immediately. */
	k_work_submit(&motion_work);
}

/* ── Bluetooth ready callback ────────────────────────────────────────────── */

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return;
	}

	/* Build static advertising entries (flags and device name). */
	const struct bt_data ad_flags[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS,
			      BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	};
	const struct bt_data ad_name[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE,
			CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	};

	ad[0] = ad_flags[0];
	ad[1] = ad_name[0];

	/* Create extended advertising sets — no per-PDU callback needed. */
	err = bt_le_ext_adv_create(&adv_fast, NULL, &ext_adv_fast_h);
	if (err) {
		LOG_ERR("bt_le_ext_adv_create (fast) failed: %d", err);
		return;
	}

	err = bt_le_ext_adv_create(&adv_slow, NULL, &ext_adv_slow_h);
	if (err) {
		LOG_ERR("bt_le_ext_adv_create (slow) failed: %d", err);
		return;
	}

	/* Encode initial payload: motion=0, pkt_id=0. */
	current_motion = false;
	ext_adv_active  = ext_adv_slow_h;

	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id);
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, false);
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[2]);

	err = bt_le_ext_adv_set_data(ext_adv_slow_h, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("bt_le_ext_adv_set_data failed: %d", err);
		return;
	}

	/* num_events=0: controller advertises continuously at the set interval. */
	struct bt_le_ext_adv_start_param start = { .num_events = 0 };

	err = bt_le_ext_adv_start(ext_adv_slow_h, &start);
	if (err) {
		LOG_ERR("Initial bt_le_ext_adv_start failed: %d", err);
		return;
	}

	/* Start period timer — fires at slow interval to update pkt_id. */
	k_timer_start(&adv_period_timer,
		      K_MSEC(ADV_SLOW_INTERVAL_MS),
		      K_MSEC(ADV_SLOW_INTERVAL_MS));

	LOG_INF("BThome PIR advertising started (idle, slow interval, timer active)");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
	int err;

	LOG_INF("BThome PIR sample starting");

	/* ── TX LED setup ───────────────────────────────────────────────── */
#if HAS_LED_TX
	if (!gpio_is_ready_dt(&led_tx)) {
		LOG_ERR("TX LED GPIO device not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE);
	LOG_INF("TX LED ready on %s pin %d", led_tx.port->name, led_tx.pin);
#endif

	/* ── PIR GPIO setup ─────────────────────────────────────────────── */
	if (!gpio_is_ready_dt(&pir)) {
		LOG_ERR("PIR GPIO device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&pir, GPIO_INPUT);
	if (err) {
		LOG_ERR("gpio_pin_configure_dt failed: %d", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&pir, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("gpio_pin_interrupt_configure_dt failed: %d", err);
		return err;
	}

	gpio_init_callback(&pir_cb_data, pir_isr, BIT(pir.pin));
	gpio_add_callback(pir.port, &pir_cb_data);

	LOG_INF("PIR sensor ready on %s pin %d", pir.port->name, pir.pin);

	/* ── BThome context init ─────────────────────────────────────────── */
	bthome_v2_init(&bthome, false, false); /* Regular device → header 0x40 */

	/* ── Bluetooth init ──────────────────────────────────────────────── */
	err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	/* Main thread has nothing to do — all work driven by interrupts. */
	k_sleep(K_FOREVER);
	return 0;
}

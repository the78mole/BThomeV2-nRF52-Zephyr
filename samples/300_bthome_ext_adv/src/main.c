/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 Extended Advertising Sample — 300_bthome_ext_adv
 * ===========================================================
 * Zielplattform: Seeed Studio XIAO nRF52840 Sense
 *
 * Überblick
 * ---------
 * Sendet alle 7,5 Sekunden ein BThome-V2-Keep-Alive-Paket (Temp, VBAT,
 * VDD, VBUS-Status, No-Motion) via Extended Advertising (num_events=1).
 * Bei PIR-Bewegung: sofort 5 Pakete mit Motion=1 (gleiche Packet-ID →
 * Empfänger-Deduplizierung), dann 1 Paket No-Motion, Timer-Reset.
 * Alle 6 Sendezyklen (= 60 s) werden ADC und Temperatur neu gemessen.
 *
 * State-Machine
 * -------------
 *   IDLE  ──────(k_timer SEND_TIMER feuert)──────► SEND_KEEPALIVE
 *   IDLE  ──────(PIR-ISR)────────────────────────► SEND_MOTION
 *   SEND_KEEPALIVE ──(ext_adv .sent CB)──────────► IDLE
 *   SEND_MOTION    ──(5× .sent + 1× no-motion)──► IDLE
 *
 * Power-Strategie
 * ---------------
 *   BT bleibt dauerhaft aktiv (bt_enable einmalig).
 *   Main-Thread schläft in k_sem_take(K_FOREVER) → CPU im WFI-Modus.
 *   Extended Advertising mit num_events=1 → Controller schaltet TX nach
 *   exakt 1 Paket ab; kein manuelles bt_le_ext_adv_stop() nötig.
 *   HFXO wird nur während des TX-Slots eingeschaltet (~100 µs).
 *   Schlafstrom: ~10–45 µA (LFRC aktiv, MPSL idle).
 *
 * Build:
 *   west build -b xiao_ble_sense samples/300_bthome_ext_adv
 *   oder: make 300-build BOARD=xiao_ble_sense
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/adc/nrf-adc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

#include <bthome_v2/bthome_v2.h>

LOG_MODULE_REGISTER(bthome_ext, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════════════════════
 * Timing-Konstanten
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Keep-Alive-Intervall: alle 7,5 Sekunden ein Paket senden. */
#define SEND_INTERVAL_MS      7500U

/** Messung alle N Sendezyklen: ADC + Chip-Temperatur aktualisieren.
 * N MUSS eine Zweierpotenz (2^n) sein – die Prüfung nutzt eine AND-Maske
 * statt Modulo: (count & (N-1)) == 0.
 * 2^3 = 8 × 7,5 s = 60 s Mess-Intervall. */
#define MEASURE_EVERY_N_SENDS 8U
/** AND-Maske für die Mess-Intervall-Prüfung (= MEASURE_EVERY_N_SENDS - 1).
 * Vorberechnet im Präprozessor; kein Laufzeit-Overhead. */
#define MEASURE_SEND_MASK     (MEASURE_EVERY_N_SENDS - 1U)

/* Compile-Time-Prüfung: MEASURE_EVERY_N_SENDS muss eine Zweierpotenz sein.
 * Trick: (N & (N-1)) == 0 gilt genau dann wenn N eine Zweierpotenz ist. */
BUILD_ASSERT((MEASURE_EVERY_N_SENDS & MEASURE_SEND_MASK) == 0U,
	     "MEASURE_EVERY_N_SENDS muss eine Zweierpotenz sein (1,2,4,8,16,...)");

/** Minimale Pulsbreite des Measure-Indikators (D1) in ms.
 * Stellt sicher, dass der Puls auf dem PPK2 auch bei Echtzeit-Zoom sichtbar
 * ist. Wird am Ende von do_measure() per k_busy_wait erreicht. */
#define MEASURE_IND_MIN_MS    20U

/** PIR-Motion-Burst: 5 Pakete mit gleicher Packet-ID. */
#define MOTION_BURST_COUNT    5U

/* ═══════════════════════════════════════════════════════════════════════════
 * P25Q16H QSPI-Flash Deep Power-Down (GPIO Bit-Bang)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define FLASH_PIN_MOSI  20u
#define FLASH_PIN_SCK   21u
#define FLASH_PIN_CSN   25u
#define FLASH_CMD_DPD   0xB9u

static void flash_deep_power_down(void)
{
	nrf_gpio_cfg_output(FLASH_PIN_CSN);
	nrf_gpio_cfg_output(FLASH_PIN_SCK);
	nrf_gpio_cfg_output(FLASH_PIN_MOSI);
	nrf_gpio_pin_set(FLASH_PIN_CSN);
	nrf_gpio_pin_clear(FLASH_PIN_SCK);
	nrf_gpio_pin_clear(FLASH_PIN_MOSI);
	nrf_gpio_pin_clear(FLASH_PIN_CSN);
	for (int i = 7; i >= 0; i--) {
		nrf_gpio_pin_write(FLASH_PIN_MOSI, (FLASH_CMD_DPD >> i) & 1u);
		nrf_gpio_pin_set(FLASH_PIN_SCK);
		nrf_gpio_pin_clear(FLASH_PIN_SCK);
	}
	nrf_gpio_pin_set(FLASH_PIN_CSN);
	k_busy_wait(10u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO-Devices (DT-Aliase aus dem Overlay)
 * ═══════════════════════════════════════════════════════════════════════════ */
static const struct gpio_dt_spec pir =
	GPIO_DT_SPEC_GET(DT_ALIAS(pir_sensor), gpios);

#if DT_NODE_HAS_STATUS(DT_ALIAS(vbat_ctrl), okay)
static const struct gpio_dt_spec vbat_en =
	GPIO_DT_SPEC_GET(DT_ALIAS(vbat_ctrl), gpios);
#define HAS_VBAT 1
#else
#define HAS_VBAT 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(active_indicator))
static const struct gpio_dt_spec led_tx =
	GPIO_DT_SPEC_GET(DT_ALIAS(active_indicator), gpios);
#define HAS_LED_TX 1
#else
#define HAS_LED_TX 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(measure_indicator))
static const struct gpio_dt_spec led_meas =
	GPIO_DT_SPEC_GET(DT_ALIAS(measure_indicator), gpios);
#define HAS_LED_MEAS 1
#else
#define HAS_LED_MEAS 0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * ADC (SAADC) — VBAT (AIN7 / P0.31) und VDD (interne Referenz)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* VBAT-Kanal aus dem DTS (zephyr,user / io-channels = <&adc 7>) */
static const struct adc_dt_spec adc_vbat =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* VDD-Kanal: SAADC-interne Versorgung messen (kein externer Pin).
 * NRF_SAADC_VDD=9 ist die Zephyr-DT-Binding-Konstante (nrf-adc.h). */
static const struct adc_channel_cfg vdd_cfg = {
	.gain             = ADC_GAIN_1_6,
	.reference        = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
	.input_positive   = NRF_SAADC_VDD,
	.channel_id       = 1,      /* Kanal 1: reserviert für VDD */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Gecashte Messwerte (shared zwischen Messungs-Workqueue und ADV-Pfad)
 * Zugriff: nur aus dem System-Workqueue (measure_work) oder aus dem
 * Main-Thread (adv_send_*). Da beide nie gleichzeitig laufen (Semaphor-
 * gesteuert), ist kein Mutex nötig.
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct {
	uint16_t vbat_mv;    /* VBAT in Millivolt (Spannungsteiler × 2) */
	uint16_t vdd_mv;     /* VDD 3V3-Rail in Millivolt */
	int16_t  temp_cdegc; /* Chip-Temperatur in 0.01 °C (z. B. 2500 = 25 °C) */
	bool     vbus;       /* USB VBUS erkannt */
} cache;

/* ═══════════════════════════════════════════════════════════════════════════
 * BThome-Kontext + Extended Advertising
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct bthome_v2_ctx bthome;
static struct bt_le_ext_adv *ext_adv;

/** Packet-ID: wird nach jedem vollständigen Zyklus um 1 erhöht.
 *  Während des Motion-Bursts bleibt sie konstant (Deduplizierung). */
static uint8_t pkt_id;

/* ═══════════════════════════════════════════════════════════════════════════
 * Wakeup-Semaphor und atomare Flags
 * ═══════════════════════════════════════════════════════════════════════════ */
static K_SEM_DEFINE(wakeup_sem, 0, 1);

/* Flags: welche Wakeup-Quelle hat den Semaphor gegeben? */
#define FLAG_SEND_TIMER  BIT(0)  /* 10s Keep-Alive-Timer */
#define FLAG_PIR         BIT(1)  /* PIR-Interrupt */
static ATOMIC_DEFINE(wakeup_flags, 2);

/** Zähler der Keep-Alive-Sendezyklen; Messung alle MEASURE_EVERY_N_SENDS. */
static uint8_t send_count;

/* ═══════════════════════════════════════════════════════════════════════════
 * Semaphor für Extended-Adv-Abschluss (.sent Callback)
 * ═══════════════════════════════════════════════════════════════════════════ */
static K_SEM_DEFINE(adv_done_sem, 0, 1);

/* ═══════════════════════════════════════════════════════════════════════════
 * Extended Advertising Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */
static void ext_adv_sent_cb(struct bt_le_ext_adv *adv,
			    struct bt_le_ext_adv_sent_info *info)
{
	ARG_UNUSED(adv);
	ARG_UNUSED(info);
	/* Signalisiert dem Main-Thread, dass num_events=1 gesendet wurde. */
	k_sem_give(&adv_done_sem);
}

static const struct bt_le_ext_adv_cb ext_adv_cb = {
	.sent = ext_adv_sent_cb,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PIR-Interrupt
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct gpio_callback pir_cb_data;

static void pir_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Interrupt sofort disarmen.
	 * EDGE_TO_ACTIVE fällt nach dem Disarmen sofort zurück auf "armed"
	 * (weil kein Pending-Event mehr aufläuft). Dennoch disarmen wir
	 * explizit, um mehrfaches Feuern während des Bursts zu verhindern.
	 * Wird am Ende des Motion-Burst-Zyklus im Main-Thread wieder aktiviert. */
	gpio_pin_interrupt_configure_dt(&pir, GPIO_INT_DISABLE);

	atomic_set_bit(wakeup_flags, FLAG_PIR);
	k_sem_give(&wakeup_sem);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 10s Keep-Alive-Timer
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct k_timer send_timer;

static void send_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	atomic_set_bit(wakeup_flags, FLAG_SEND_TIMER);
	k_sem_give(&wakeup_sem);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ADC-Messung: VBAT und VDD
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Liest VBAT (AIN7 / P0.31) über den On-Board-Spannungsteiler.
 * Der Teiler R1=1MΩ / R2=1MΩ halbiert die Spannung → × 2 für echte VBAT.
 * P0.14 wird vor der Messung LOW gesetzt (Enable) und danach DISCONNECTED
 * (hochohmig), um Leckströme über den Teiler zu vermeiden.
 *
 * Rückgabe: VBAT in Millivolt (0 bei Fehler).
 */
static uint16_t measure_vbat_mv(void)
{
#if HAS_VBAT
	int16_t raw = 0;
	int ret;

	/* Spannungsteiler einschalten (P0.14 → LOW = ACTIVE_LOW GPIO) */
	gpio_pin_configure_dt(&vbat_en, GPIO_OUTPUT_ACTIVE);
	/* Kurze Einschwingzeit */
	k_busy_wait(200u);

	struct adc_sequence seq = {
		.channels    = BIT(adc_vbat.channel_id),
		.buffer      = &raw,
		.buffer_size = sizeof(raw),
		.resolution  = 12,
	};
	ret = adc_read(adc_vbat.dev, &seq);

	/* Spannungsteiler hochohmig schalten → kein Leckstrom */
	gpio_pin_configure_dt(&vbat_en, GPIO_DISCONNECTED);

	if (ret < 0) {
		LOG_WRN("VBAT ADC read failed: %d", ret);
		return 0U;
	}

	/* raw → mV: interne Referenz = 600 mV, Gain = 1/6 → VFS = 3600 mV
	 * ADC-Wert [12-bit] = raw * 3600 / 4096
	 * VBAT = gemessene Spannung × 2 (Spannungsteiler) */
	int32_t mv = (int32_t)raw * 3600 / 4096 * 2;

	return (mv > 0) ? (uint16_t)mv : 0U;
#else
	return 0U;
#endif
}

/**
 * Liest die interne nRF52-Versorgungsspannung (VDD) über den SAADC.
 * Kein externer Pin nötig; NRF_SAADC_INPUT_VDD wird intern verbunden.
 *
 * Rückgabe: VDD in Millivolt (0 bei Fehler).
 */
static uint16_t measure_vdd_mv(void)
{
	const struct device *adc_dev = adc_vbat.dev;
	int16_t raw = 0;
	int ret;

	ret = adc_channel_setup(adc_dev, &vdd_cfg);
	if (ret < 0) {
		LOG_WRN("VDD channel setup: %d", ret);
		return 0U;
	}

	struct adc_sequence seq = {
		.channels    = BIT(vdd_cfg.channel_id),
		.buffer      = &raw,
		.buffer_size = sizeof(raw),
		.resolution  = 12,
	};
	ret = adc_read(adc_dev, &seq);
	if (ret < 0) {
		LOG_WRN("VDD ADC read failed: %d", ret);
		return 0U;
	}

	/* VFS = 3600 mV (Gain 1/6, Ref 600 mV) */
	int32_t mv = (int32_t)raw * 3600 / 4096;

	return (mv > 0) ? (uint16_t)mv : 0U;
}

/**
 * Liest die interne Chip-Temperatur des nRF52840.
 * Der TEMP-Peripheral liefert die Temperatur in 0.25 °C Schritten (Einheit:
 * 1/4 °C) als 32-Bit-Integer.  Konvertierung in 0.01 °C für BThome (0x02).
 *
 * Rückgabe: Temperatur in 0.01 °C (z. B. 2500 = 25.00 °C).
 */
static int16_t measure_chip_temp_cdegc(void)
{
	/* TEMP-Register: 1 LSB = 0.25 °C → × 100 / 4 = × 25 */
	NRF_TEMP->TASKS_START = 1;
	while (!NRF_TEMP->EVENTS_DATARDY) {
		/* busy-wait: typisch < 100 µs */
	}
	NRF_TEMP->EVENTS_DATARDY = 0;
	int32_t raw = (int32_t)NRF_TEMP->TEMP;   /* signed 32-bit, Q7.2 */
	NRF_TEMP->TASKS_STOP = 1;

	/* raw × 25 = 0.01 °C; Sättigung auf sint16-Grenzen */
	int32_t cdegc = raw * 25;

	if (cdegc > INT16_MAX) {
		cdegc = INT16_MAX;
	} else if (cdegc < INT16_MIN) {
		cdegc = INT16_MIN;
	}
	return (int16_t)cdegc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Messung aller Werte und Aktualisierung des Cache
 * ═══════════════════════════════════════════════════════════════════════════ */
static void do_measure(void)
{
#if HAS_LED_MEAS
	gpio_pin_set_dt(&led_meas, 1);
#endif
	cache.vbat_mv    = measure_vbat_mv();
	cache.vdd_mv     = measure_vdd_mv();
	cache.temp_cdegc = measure_chip_temp_cdegc();
	cache.vbus       = (nrf_power_usbregstatus_get(NRF_POWER) &
			    NRF_POWER_USBREGSTATUS_VBUSDETECT_MASK) != 0;

	LOG_INF("Measure: vbat=%u mV  vdd=%u mV  temp=%d 0.01°C  vbus=%d",
		cache.vbat_mv, cache.vdd_mv, cache.temp_cdegc,
		(int)cache.vbus);

	/* Mindest-Pulsbreite sicherstellen → sichtbar auf PPK2 */
#if HAS_LED_MEAS
	k_msleep(MEASURE_IND_MIN_MS);
	gpio_pin_set_dt(&led_meas, 0);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BThome-Paket zusammenbauen und per Extended Advertising senden
 *
 * num_events=1 → Controller sendet genau ein ADV_EXT_IND und schaltet
 * dann automatisch ab.  Der .sent-Callback gibt adv_done_sem frei.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void adv_send_packet(bool motion, uint8_t id)
{
	struct bt_le_ext_adv_start_param start = {
		.timeout    = 0,     /* kein Timeout (num_events begrenzt) */
		.num_events = 1,     /* exakt 1 Paket senden, dann stoppen */
	};

	/* BThome-Payload aufbauen */
	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, id);                       /* 0x00 */
	bthome_v2_add_temperature(&bthome, cache.temp_cdegc);       /* 0x02 */
	bthome_v2_add_voltage(&bthome, cache.vbat_mv);              /* 0x0C */
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_POWER_BIN,
			     cache.vbus);                           /* 0x10 */
	bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, motion);  /* 0x21 */
	bthome_v2_encode(&bthome);

	/* Service-Data-AD aktualisieren; Flags + Name werden mit jedem
	 * Paket mitgesendet, da das Set non-scannable ist (kein Scan-Response).
	 * Payload: 3B Flags + 10B Name + 17B Service Data = 30B ≤ 31B Limit. */
	struct bt_data ad[3] = {
		BT_DATA_BYTES(BT_DATA_FLAGS,
			      BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_NAME_COMPLETE,
			CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
		{ 0 },  /* wird von bthome_v2_get_bt_data befüllt */
	};
	bthome_v2_get_bt_data(&bthome, &ad[2]);

	int ret = bt_le_ext_adv_set_data(ext_adv, ad, ARRAY_SIZE(ad), NULL, 0);

	if (ret) {
		LOG_ERR("ext_adv_set_data: %d", ret);
		return;
	}

	/* Semaphor auf 0 setzen, bevor wir starten */
	k_sem_reset(&adv_done_sem);

#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 1);
#endif

	ret = bt_le_ext_adv_start(ext_adv, &start);
	if (ret) {
		LOG_ERR("ext_adv_start: %d", ret);
#if HAS_LED_TX
		gpio_pin_set_dt(&led_tx, 0);
#endif
		return;
	}

	/* Warten bis .sent-Callback signalisiert (num_events=1 wurde gesendet).
	 * Timeout 500 ms als Safety-Net: hängt der Controller, wird LED zurückgesetzt
	 * und die Funktion kehrt zurück statt ewig zu blockieren. */
	if (k_sem_take(&adv_done_sem, K_MSEC(500)) != 0) {
		LOG_WRN("adv_done timeout – Controller hängt?");
	}

#if HAS_LED_TX
	gpio_pin_set_dt(&led_tx, 0);
#endif

	LOG_INF("ADV sent: motion=%d id=%u", (int)motion, id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
	int ret;

	LOG_INF("=== BThome Ext Adv Sample ===");

	/* ── Flash in Deep Power-Down (vor bt_enable) ──────────────────── */
	flash_deep_power_down();

	/* ── GPIO initialisieren ────────────────────────────────────────── */
	if (!gpio_is_ready_dt(&pir)) {
		LOG_ERR("PIR GPIO nicht bereit");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&pir, GPIO_INPUT);
	gpio_init_callback(&pir_cb_data, pir_isr, BIT(pir.pin));
	gpio_add_callback_dt(&pir, &pir_cb_data);

#if HAS_VBAT
	if (!gpio_is_ready_dt(&vbat_en)) {
		LOG_WRN("VBAT-Enable GPIO nicht bereit");
	}
	/* Initial hochohmig → kein Leckstrom */
	gpio_pin_configure_dt(&vbat_en, GPIO_DISCONNECTED);
#endif

#if HAS_LED_TX
	if (gpio_is_ready_dt(&led_tx)) {
		gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE);
	}
#endif

#if HAS_LED_MEAS
	if (gpio_is_ready_dt(&led_meas)) {
		gpio_pin_configure_dt(&led_meas, GPIO_OUTPUT_INACTIVE);
	}
#endif

	/* ── ADC initialisieren ─────────────────────────────────────────── */
	if (!adc_is_ready_dt(&adc_vbat)) {
		LOG_ERR("ADC nicht bereit");
		return -ENODEV;
	}
	ret = adc_channel_setup_dt(&adc_vbat);
	if (ret < 0) {
		LOG_ERR("ADC Kanal Setup: %d", ret);
		return ret;
	}

	/* ── BThome-Kontext initialisieren ─────────────────────────────── */
	bthome_v2_init(&bthome, false, false);

	/* ── BLE einmalig aktivieren ────────────────────────────────────── */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("bt_enable: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth bereit");

	/* ── Extended Advertising Set anlegen ──────────────────────────── */
	/*
	 * BT_LE_ADV_OPT_EXT_ADV wird NICHT gesetzt → Legacy-Advertising-Set.
	 * bt_le_ext_adv_create erstellt damit einen Legacy-ADV-Set (PDU-Typ:
	 * ADV_NONCONN_IND auf Primary Channels 37/38/39). Das ist vollständig
	 * kompatibel mit BThome / Home Assistant und dem bthome-logger.
	 * num_events=1 und der .sent-Callback funktionieren auch mit Legacy-Sets.
	 * EXT_ADV würde AUX_ADV_IND auf Secondary Channels verwenden → unsichtbar
	 * für Scanner ohne BT5-Extended-Scan-Support.
	 */
	struct bt_le_adv_param adv_param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
				     BT_GAP_ADV_FAST_INT_MIN_2,
				     BT_GAP_ADV_FAST_INT_MAX_2,
				     NULL);

	ret = bt_le_ext_adv_create(&adv_param, &ext_adv_cb, &ext_adv);
	if (ret) {
		LOG_ERR("ext_adv_create: %d", ret);
		return ret;
	}

	/* ── Erste Messung sofort durchführen ───────────────────────────── */
	do_measure();

	/* ── Timer starten ──────────────────────────────────────────────── */
	k_timer_init(&send_timer, send_timer_cb, NULL);

	k_timer_start(&send_timer, K_MSEC(SEND_INTERVAL_MS),
			       K_MSEC(SEND_INTERVAL_MS));

	/* PIR-Interrupt aktivieren.
	 * EDGE_TO_ACTIVE: fällt nur auf die steigende Flanke an.
	 * Dadurch kein sofortiges Wiederfeuern wenn PIR beim Re-Armen noch HIGH ist. */
	gpio_pin_interrupt_configure_dt(&pir, GPIO_INT_EDGE_TO_ACTIVE);

	/* Erstes Keep-Alive sofort senden */
	adv_send_packet(false, pkt_id++);

	/* ══════════════════════════════════════════════════════════════════
	 * Haupt-Schleife: CPU schläft auf Semaphor, wird von Timer oder ISR
	 * geweckt.
	 * ══════════════════════════════════════════════════════════════════ */
	while (true) {
		/* CPU schläft im WFI-Modus bis ein Flag gesetzt wird */
		k_sem_take(&wakeup_sem, K_FOREVER);

		/* ── PIR-Motion-Burst ─────────────────────────────────────── */
		if (atomic_test_and_clear_bit(wakeup_flags, FLAG_PIR)) {
			/*
			 * 5 Pakete mit GLEICHER pkt_id senden.
			 * Empfänger-seitige Deduplizierung verhindert mehrfaches
			 * Auslösen bei Paketen mit identischer ID.
			 * Die Packet-ID wird während des Bursts NICHT erhöht.
			 */
			uint8_t motion_id = pkt_id;  /* einfrieren */

			for (uint8_t i = 0; i < MOTION_BURST_COUNT; i++) {
				adv_send_packet(true, motion_id);
			}

			/* Nach dem Burst: No-Motion + Packet-ID erhöhen */
			pkt_id = (uint8_t)(motion_id + 1U);
			adv_send_packet(false, pkt_id++);

			/* Send-Timer neu starten: nächstes Keep-Alive in genau
			 * SEND_INTERVAL_MS ab diesem Moment. */
			k_timer_start(&send_timer,
				      K_MSEC(SEND_INTERVAL_MS),
				      K_MSEC(SEND_INTERVAL_MS));

			/* Konsumiere einen eventuell gleichzeitig gesetzten
			 * SEND_TIMER-Flag, da wir gerade gesendet haben. */
			atomic_clear_bit(wakeup_flags, FLAG_SEND_TIMER);

			/* PIR-Interrupt wieder scharfstellen (Flanken-getriggert) */
			gpio_pin_interrupt_configure_dt(&pir,
							GPIO_INT_EDGE_TO_ACTIVE);
			continue;
		}

		/* ── Keep-Alive senden ────────────────────────────────────── */
		if (atomic_test_and_clear_bit(wakeup_flags, FLAG_SEND_TIMER)) {
			/* Alle MEASURE_EVERY_N_SENDS Zyklen vor dem Senden messen.
			 * AND-Maske statt Modulo (N ist Zweierpotenz): */
			if ((++send_count & MEASURE_SEND_MASK) == 0U) {
				do_measure();
			}
			adv_send_packet(false, pkt_id++);
		}
	}

	return 0;
}

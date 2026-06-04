/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 IMU Sample
 * ====================
 * Liest den eingebauten 6-Achsen-IMU (LSM6DS3TR-C) des Seeed XIAO
 * nRF52840 Sense und sendet Beschleunigung (X/Y/Z) und Gyroskop (Z)
 * alle 5 Sekunden als BThome-V2-Advertising.
 *
 * BThome-Objekte (in aufsteigender ID-Reihenfolge, Spec-Anforderung):
 *   0x00  Packet ID           uint8
 *   0x52  Gyroscope           uint16   factor 0.001     °/s   (1×, |ω| Betrag)
 *   0x63  Acceleration axis   sint32   factor 0.000001  m/s²  (3×, X/Y/Z)
 *
 * Nutzlast-Budget (23 Byte max ohne Name im ADV):
 *   0x00  pkt_id   : 2 B
 *   0x52  gyro_mag : 3 B
 *   0x63  accel_x  : 5 B
 *   0x63  accel_y  : 5 B
 *   0x63  accel_z  : 5 B    Gesamt: 20 B ✓
 *
 * Takt:
 *   IMU aktiv → fetch → encode → ADV 1 s → ADV stoppen → sleep 4 s
 *   Netto-Zykluszeit ≈ 5 s
 *
 * Sensor-Konvertierung:
 *   Beschleunigung: Zephyr sensor_value (m/s²) → sint32 (µm/s² = × 1e6)
 *   Gyroskop:       Zephyr sensor_value (rad/s) → uint16 (mdeg/s)
 *                   Formel: |val_urad/s| × 180000 / 3141593
 *
 * Build:
 *   west build -b xiao_ble_sense samples/200_bthome_imu
 *   oder: make 200-build BOARD=xiao_ble_sense
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <hal/nrf_gpio.h>
#include <math.h>

#include <bthome_v2/bthome_v2.h>

LOG_MODULE_REGISTER(bthome_imu, LOG_LEVEL_INF);

/* ── IMU-Regulator-Startup-Delay ────────────────────────────────────────
 * Der regulator-fixed-Treiber setzt P1.08 HIGH während regulator_fixed_init()
 * (SYS_INIT POST_KERNEL, Prio 75). In der regulator-boot-on-Code-Pfad wird
 * startup-delay-us NICHT abgewartet (nur im api->enable()-Pfad). Der
 * LSM6DSL-Treiber (Prio 90) würde sonst unmittelbar danach starten, bevor
 * der IMU die 3 ms Hochlaufzeit abgeschlossen hat.
 *
 * Lösung: SYS_INIT bei Prio 80 wartet 5 ms, bevor der Sensor-Treiber
 * bei Prio 90 initialisiert wird.
 */
static int imu_power_on_delay(void)
{
	/* 5 ms busy-wait: sicherer als k_sleep() während POST_KERNEL-Init */
	k_busy_wait(5000U);
	return 0;
}
SYS_INIT(imu_power_on_delay, POST_KERNEL, 80);

/* ── P25Q16H QSPI-Flash Deep Power-Down via GPIO Bit-Bang ──────────────── */
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

/* ── IMU-Gerät ─────────────────────────────────────────────────────────── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));
#define HAS_IMU 1
#else
#define HAS_IMU 0
#warning "kein imu0-Alias definiert – Sensor-Ausgabe wird simuliert"
#endif

/* ── Active Indicator (P0.05 = D5): HIGH während BLE aktiv ─────────────── */
#if DT_NODE_HAS_STATUS(DT_ALIAS(active_indicator), okay)
static const struct gpio_dt_spec led_tx =
	GPIO_DT_SPEC_GET(DT_ALIAS(active_indicator), gpios);
#define HAS_LED_TX 1
#else
#define HAS_LED_TX 0
#endif

/* ── Timing ────────────────────────────────────────────────────────────── */
/** Aktive Advertising-Dauer pro Zyklus (ms) */
#define ADV_BURST_MS     1000U
/** Schlafzeit nach dem Advertising (ms), ergibt ~5 s Gesamtperiode */
#define SLEEP_MS         4000U

/* ── BLE-Parameter ─────────────────────────────────────────────────────── */
#define ADV_INTERVAL_MS  200U
#define ADV_INT          (ADV_INTERVAL_MS * 8U / 5U)  /* BLE-Einheiten */

static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
			     ADV_INT, ADV_INT, NULL);

/* ── BThome-Kontext ─────────────────────────────────────────────────────── */
static struct bthome_v2_ctx bthome;
/* ad[0] = Flags, ad[1] = Service Data (kein Name im ADV → Scan Response) */
static struct bt_data ad[2];

/* ── Scan Response: Gerätename ──────────────────────────────────────────── */
static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ── Paketzhähler ──────────────────────────────────────────────────────── */
static uint8_t pkt_id;

/* ── Sensor-Konvertierungshelfer ────────────────────────────────────────── */

/**
 * Wandelt einen Zephyr sensor_value (val1 + val2/1e6 in m/s²)
 * in den BThome-0x63-Rohwert (sint32, × 1e-6 m/s²) um.
 */
static int32_t accel_to_micro_ms2(const struct sensor_value *v)
{
	return (int32_t)((int64_t)v->val1 * 1000000 + v->val2);
}

/**
 * Wandelt einen Zephyr sensor_value (rad/s) in milli-Grad/s (uint16) um.
 * Klammerung auf uint16-Wertebereich (max 65535 mdeg/s = 65.535 °/s).
 */
static uint16_t gyro_to_milli_degs(const struct sensor_value *v)
{
	/* Gesamtwert in µrad/s */
	int64_t uras = (int64_t)v->val1 * 1000000 + v->val2;
	/* µrad/s → mdeg/s: × 180000 / 3141593 */
	int64_t mdeg = uras * 180000LL / 3141593LL;

	if (mdeg < 0) {
		mdeg = -mdeg;
	}
	if (mdeg > 65535) {
		mdeg = 65535;
	}
	return (uint16_t)mdeg;
}

/**
 * Berechnet den euklidischen Betrag |ω| = √(gx² + gy² + gz²) in mdeg/s.
 * Da BThome-Gyro (0x52) vorzeichenlos ist, ist der Vektorbetrag die
 * semantisch korrekte Darstellung der Rotationsgeschwindigkeit.
 *
 * Hinweis: Bei gleichzeitiger Vollaussteuerung aller 3 Achsen kann
 * |ω| ≤ √3 × 65535 ≈ 113 511 mdeg/s > UINT16_MAX → Sättigung bei 65535.
 */
static uint16_t gyro_magnitude(uint16_t gx, uint16_t gy, uint16_t gz)
{
	uint64_t sq = (uint64_t)gx * gx + (uint64_t)gy * gy + (uint64_t)gz * gz;
	uint32_t mag = (uint32_t)sqrtf((float)sq);

	return (mag > UINT16_MAX) ? UINT16_MAX : (uint16_t)mag;
}

/* ── Advertising-Helfer ─────────────────────────────────────────────────── */
static void adv_send(int32_t ax, int32_t ay, int32_t az, uint16_t gz)
{
	int err;

	bt_le_adv_stop();

	bthome_v2_clear(&bthome);
	bthome_v2_add_packet_id(&bthome, pkt_id++);    /* 0x00 */
	bthome_v2_add_gyroscope(&bthome, gz);          /* 0x52 */
	bthome_v2_add_acceleration_axis(&bthome, ax);  /* 0x63 */
	bthome_v2_add_acceleration_axis(&bthome, ay);  /* 0x63 */
	bthome_v2_add_acceleration_axis(&bthome, az);  /* 0x63 */
	bthome_v2_encode(&bthome);
	bthome_v2_get_bt_data(&bthome, &ad[1]);

	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("bt_le_adv_start: %d", err);
		return;
	}

	LOG_INF("ADV pkt=%u  ax=%d ay=%d az=%d µm/s²  gz=%u mdeg/s",
		pkt_id - 1, ax, ay, az, gz);
}

/* ── main ──────────────────────────────────────────────────────────────── */
int main(void)
{
	LOG_INF("=== BThome IMU Sample ===");

	/* Flash in Deep Power-Down, bevor HFXO durch BLE gestartet wird */
	flash_deep_power_down();

#if HAS_IMU
	if (!device_is_ready(imu)) {
		LOG_ERR("IMU nicht bereit: %s", imu->name);
		return -ENODEV;
	}
	LOG_INF("IMU bereit: %s", imu->name);
#endif

	/* BThome-Kontext + Flags-AD */
	bthome_v2_init(&bthome, false, false);
	ad[0] = (struct bt_data) BT_DATA_BYTES(BT_DATA_FLAGS,
			BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);

	/* Active Indicator initialisieren (P0.05 = D5, active-high, initial LOW) */
#if HAS_LED_TX
	if (gpio_is_ready_dt(&led_tx)) {
		gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE);
	}
#endif

	/* Bluetooth einmalig aktivieren */
	int ret = bt_enable(NULL);

	if (ret) {
		LOG_ERR("bt_enable: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth bereit");

	/*
	 * Hauptschleife: alle ~5 s messen und advertisen
	 *
	 * Low-Power-Strategie:
	 *   1. BLE nach dem ADV-Fenster mit bt_disable() vollständig abschalten
	 *      → SoftDevice Controller + HFXO gehen aus → spart ~2.5 mA
	 *   2. IMU per PM in Power-Down versetzen (CTRL1_XL/CTRL2_G ODR=0)
	 *      → spart ~0.9 mA, ODR wird beim Resume automatisch wiederhergestellt
	 *   3. TICKLESS_KERNEL lässt den nRF52840 während k_sleep() in
	 *      System Idle (WFE) verweilen → nur ~3 µA SoC-Strom
	 *
	 * Stromverbrauch (geschätzt, XIAO BLE Sense):
	 *   ADV-Phase (1 s):    ~6 mA  (BLE TX + IMU aktiv)
	 *   Schlaf-Phase (4 s): ~45 µA (HW-Floor: Charge-IC + LDO)
	 *   Mittlerer Strom:    ~1.4 mA → CR2032  ~7 Tage
	 *                               → 2× AA  ~75 Tage
	 */
	while (true) {
		int32_t ax = 0, ay = 0, az = 0;
		uint16_t gx = 0, gy = 0, gz = 0, gmag = 0;

#if HAS_IMU
		ret = sensor_sample_fetch(imu);
		if (ret) {
			LOG_WRN("sensor_sample_fetch: %d", ret);
		} else {
			struct sensor_value sv_ax, sv_ay, sv_az;
			struct sensor_value sv_gx, sv_gy, sv_gz;

			sensor_channel_get(imu, SENSOR_CHAN_ACCEL_X, &sv_ax);
			sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Y, &sv_ay);
			sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Z, &sv_az);
			sensor_channel_get(imu, SENSOR_CHAN_GYRO_X,  &sv_gx);
			sensor_channel_get(imu, SENSOR_CHAN_GYRO_Y,  &sv_gy);
			sensor_channel_get(imu, SENSOR_CHAN_GYRO_Z,  &sv_gz);

			ax = accel_to_micro_ms2(&sv_ax);
			ay = accel_to_micro_ms2(&sv_ay);
			az = accel_to_micro_ms2(&sv_az);
			gx = gyro_to_milli_degs(&sv_gx);
			gy = gyro_to_milli_degs(&sv_gy);
			gz = gyro_to_milli_degs(&sv_gz);
			gmag = gyro_magnitude(gx, gy, gz);

			LOG_INF("Accel:    X=%d Y=%d Z=%d µm/s²", ax, ay, az);
			LOG_INF("Gyro XYZ: X=%u Y=%u Z=%u mdeg/s", gx, gy, gz);
			LOG_INF("Gyro |ω|: %u mdeg/s  (%u.%03u °/s)  → BThome",
				gmag, gmag / 1000U, gmag % 1000U);
		}
#endif

		/* Active Indicator HIGH: BLE-Fenster beginnt */
#if HAS_LED_TX
		gpio_pin_set_dt(&led_tx, 1);
#endif

		adv_send(ax, ay, az, gmag);

		/* Advertising-Fenster */
		k_sleep(K_MSEC(ADV_BURST_MS));

		/* ── Schlafphase: alle Verbraucher abschalten ── */

		/* bt_le_adv_stop() muss VOR bt_disable() gerufen werden.
		 * Bei Fehler wird trotzdem weitergemacht – bt_disable() räumt auf. */
		ret = bt_le_adv_stop();
		if (ret && ret != -EALREADY) {
			LOG_WRN("bt_le_adv_stop: %d", ret);
		}

		/* IMU in Hardware-Power-Down (ODR → 0, ~6 µA Ruhestrom) */
#if HAS_IMU
		ret = pm_device_action_run(imu, PM_DEVICE_ACTION_SUSPEND);
		if (ret && ret != -ENOTSUP && ret != -EALREADY) {
			LOG_WRN("IMU SUSPEND: %d", ret);
		}
#endif

		/* Active Indicator LOW: BLE-Fenster abgeschlossen */
#if HAS_LED_TX
		gpio_pin_set_dt(&led_tx, 0);
#endif

		/* BLE-Stack + HFXO vollständig abschalten (~2.5 mA gespart).
		 * bt_disable() gibt HFCLK frei; MPSL bleibt initialisiert
		 * (kein BT_UNINIT_MPSL_ON_DISABLE!) → RC-Kalibrierung läuft korrekt. */
		ret = bt_disable();
		if (ret) {
			LOG_ERR("bt_disable: %d – sleep skipped", ret);
			/* HFXO nicht freigegeben → nicht schlafen, sofort neu starten */
			goto wake_up;
		}

		/* nRF52840 schläft tickless (WFE) → ~3 µA SoC-Strom */
		k_sleep(K_MSEC(SLEEP_MS));

wake_up:
		/* ── Aufwecken: Verbraucher wieder einschalten ── */

		/* BLE neu initialisieren (~100–300 ms HFXO-Startzeit einkalkuliert) */
		ret = bt_enable(NULL);
		if (ret) {
			LOG_ERR("bt_enable (loop): %d", ret);
			/* Ohne BLE weiter laufen – nächste Iteration versucht es erneut */
		}

		/* IMU aufwecken: Treiber stellt gespeicherten ODR wieder her */
#if HAS_IMU
		ret = pm_device_action_run(imu, PM_DEVICE_ACTION_RESUME);
		if (ret && ret != -ENOTSUP && ret != -EALREADY) {
			LOG_WRN("IMU RESUME: %d", ret);
		}
		/* 10 ms warten: sicherer als 2 ms (1/ODR bei 104 Hz = 9.6 ms) */
		k_msleep(10);
#endif
	}

	return 0;
}

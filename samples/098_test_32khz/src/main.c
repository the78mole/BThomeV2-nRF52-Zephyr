/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * 098 — 32 kHz Clock Source Diagnostic
 * ======================================
 * Determines whether the XIAO nRF52840 has a real 32.768 kHz crystal fitted
 * (LFXO) or only the internal RC oscillator (LFRC).
 *
 * Messprinzip
 * ───────────
 * HFCLK (HFXO, 32 MHz Quarz × 2 = 64 MHz) läuft unabhängig vom LFCLK.
 * TIMER2 wird auf 1 MHz konfiguriert (HFCLK / 64, 1 µs Auflösung).
 * k_sleep(K_MSEC(1000)) schläft exakt 1 "LFCLK-Sekunde" (Kernel-Ticker
 * läuft auf LFCLK-Basis). In dieser Zeit zählt TIMER2 HFCLK-Takte.
 *
 * Ist LFCLK exakt 32768 Hz → TIMER2 misst 1 000 000 µs.
 * Ist LFCLK um 500 ppm zu langsam → TIMER2 misst 1 000 500 µs.
 *
 * Elegante Näherung bei 1 MHz-Timer:  ppm ≈ 1 000 000 − elapsed_µs
 *
 * Build variants
 * ──────────────
 * boards/xiao_ble_sense.conf          — RC source (always boots)
 * boards/xiao_ble_sense_xtal.conf     — XTAL source (hangs if no crystal!)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Nordic HAL für direkte Registerzugriffe */
#include <hal/nrf_clock.h>
#include <hal/nrf_rtc.h>

/* ARM Cortex-M4 DWT CYCCNT — eingebauter HFCLK-Zyklen-Zähler, kein Treiber nötig */
#include <cmsis_core.h>

LOG_MODULE_REGISTER(test_32khz, LOG_LEVEL_INF);

/* Anzahl Messfenster */
#define MEASURE_WINDOWS     5
/* Nominale LFCLK-Frequenz */
#define LFCLK_NOMINAL_HZ    32768U
/* HFCLK = 64 MHz (HFXO × 2) — Referenz für DWT CYCCNT */
#define HFCLK_HZ            64000000UL
/* RTC2: PRESCALER=0 → jeder Tick = 1 LFCLK-Zyklus
 * Gate = 32768 Ticks = exakt 1 Nominalssekunde laut LFCLK             */
#define RTC_GATE_TICKS      32768U
/* RTC-Zähler ist 24 bit */
#define RTC_COUNTER_MAX     0xFFFFFFUL

/* ── Stringify LFCLK source ──────────────────────────────────────────────── */
static const char *src_name(nrf_clock_lfclk_t src)
{
	switch (src) {
	case NRF_CLOCK_LFCLK_RC:   return "RC";
	case NRF_CLOCK_LFCLK_XTAL: return "XTAL";
	case NRF_CLOCK_LFCLK_SYNTH: return "SYNTH";
	default:                    return "UNKNOWN";
	}
}

int main(void)
{
	/* ── Countdown: Zeit zum Öffnen des Terminals ────────────────────── */
	LOG_INF("=== 32 kHz Clock Source Diagnostic ===");
	LOG_INF("Starte Messung in 3 s - bitte Terminal verbinden ...");
	k_sleep(K_SECONDS(1));
	LOG_INF("... 2 ...");
	k_sleep(K_SECONDS(1));
	LOG_INF("... 1 ...");
	k_sleep(K_SECONDS(1));

	/* ── 1. HFXO starten (exakte Referenz für TIMER2) ───────────────── */
	/* USB CDC ist aktiv → HFXO läuft bereits; trotzdem explizit anfordern */
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
	int64_t hf_deadline = k_uptime_get() + 500;

	while (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
		if (k_uptime_get() > hf_deadline) {
			LOG_WRN("HFXO-Start Timeout — Referenz ungenau (HFRC)!");
			break;
		}
		k_sleep(K_MSEC(1));
	}
	LOG_INF("HFCLK Referenz: %s",
		nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)
		? "HFXO (±50 ppm)" : "HFRC (±1%)");

	/* ── 2. LFCLK-Quelle aus Register lesen ─────────────────────────── */
	nrf_clock_lfclk_t src_req = nrf_clock_lf_src_get(NRF_CLOCK);

	LOG_INF("LFCLK source requested : %s", src_name(src_req));
	k_sleep(K_MSEC(100));

	nrf_clock_lfclk_t src_run = nrf_clock_lf_actv_src_get(NRF_CLOCK);

	LOG_INF("LFCLK source running   : %s", src_name(src_run));

	/* ── 3. SysTick als HFCLK-Referenz (64 MHz) ────────────────────── */
	/* Zephyr nutzt auf nRF52840 RTC1 als Kernel-Ticker (NRF_RTC_TIMER) */
	/* → SysTick ist frei. DWT CYCCNT ist ohne Debugger nicht nutzbar   */
	/* (TRCENA-Schreibzugriff wird auf nRF52840 ohne J-Link ignoriert).  */
	/* SysTick zählt ABWÄRTS von LOAD=0xFFFFFF (24 bit, max ~0.262 s).  */
	SysTick->CTRL = 0U;
	SysTick->LOAD = 0x00FFFFFFUL;   /* 24-bit Maximalwert */
	SysTick->VAL  = 0U;             /* Zähler zurücksetzen */
	/* CLKSOURCE=1 → Prozessortakt (64 MHz), kein Interrupt */
	SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

	/* ── 4. RTC2 als LFCLK-Gate (PRESCALER=0 → 1 Tick = 1 LFCLK) ─── */
	/* Zephyr verwendet RTC1 für den Kernel-Ticker; RTC2 ist frei.      */
	nrf_rtc_prescaler_set(NRF_RTC2, 0);
	nrf_rtc_task_trigger(NRF_RTC2, NRF_RTC_TASK_STOP);
	nrf_rtc_task_trigger(NRF_RTC2, NRF_RTC_TASK_CLEAR);
	nrf_rtc_task_trigger(NRF_RTC2, NRF_RTC_TASK_START);

	/* ── 5. LFCLK-Frequenz gegen HFCLK messen ───────────────────────── */
	LOG_INF("Messe LFCLK-Frequenz (SysTick @ 64 MHz vs RTC2-Gate) ...");
	LOG_INF("%d x %u LFCLK-Ticks Fenster:", MEASURE_WINDOWS, RTC_GATE_TICKS);

	int64_t total_ppm    = 0;
	int32_t total_err_hz = 0;

	for (int i = 0; i < MEASURE_WINDOWS; i++) {
		/* RTC2 auf Null zurücksetzen */
		nrf_rtc_task_trigger(NRF_RTC2, NRF_RTC_TASK_CLEAR);
		/* Warte bis CLEAR wirksam (max. 2 LFCLK-Zyklen ≈ 61 µs) */
		while (nrf_rtc_counter_get(NRF_RTC2) != 0) {
		}

		/* SysTick startwert lesen; Überläufe im Busy-Wait akkumulieren */
		uint32_t st_prev   = SysTick->VAL;
		uint64_t st_cycles = 0U;
		(void)SysTick->CTRL; /* COUNTFLAG löschen */

		/* Busy-Wait: Gate = RTC_GATE_TICKS LFCLK-Ticks               */
		/* SysTick zählt ABWÄRTS → Überlauf wenn VAL(now) > VAL(prev) */
		while (nrf_rtc_counter_get(NRF_RTC2) < RTC_GATE_TICKS) {
			uint32_t st_now = SysTick->VAL;

			if (st_now > st_prev) {
				/* Überlauf: 0 → 0xFFFFFF */
				st_cycles += (uint64_t)st_prev +
					     (0x1000000UL - st_now);
			} else {
				st_cycles += (uint64_t)(st_prev - st_now);
			}
			st_prev = st_now;
		}
		/* Letztes Teilstück aufaddieren */
		{
			uint32_t st_end = SysTick->VAL;

			if (st_end > st_prev) {
				st_cycles += (uint64_t)st_prev +
					     (0x1000000UL - st_end);
			} else {
				st_cycles += (uint64_t)(st_prev - st_end);
			}
		}

		/* eff. LFCLK-Freq: f = 32768 * 64e6 / st_cycles              */
		int32_t eff_hz = (int32_t)((int64_t)LFCLK_NOMINAL_HZ *
					   HFCLK_HZ / (int64_t)st_cycles);
		int32_t err_hz = eff_hz - (int32_t)LFCLK_NOMINAL_HZ;
		int32_t ppm    = (int32_t)((int64_t)err_hz * 1000000 /
					   (int32_t)LFCLK_NOMINAL_HZ);

		LOG_INF("  Fenster %d: %llu Zyklen  eff=%d Hz  err=%+d Hz  ppm=%+d",
			i + 1, (unsigned long long)st_cycles, eff_hz, err_hz, ppm);

		total_ppm    += ppm;
		total_err_hz += err_hz;
	}

	SysTick->CTRL = 0U;  /* SysTick stoppen */
	nrf_rtc_task_trigger(NRF_RTC2, NRF_RTC_TASK_STOP);

	int32_t avg_ppm    = (int32_t)(total_ppm    / MEASURE_WINDOWS);
	int32_t avg_err_hz = (int32_t)(total_err_hz / MEASURE_WINDOWS);
	int32_t avg_hz     = LFCLK_NOMINAL_HZ + avg_err_hz;

	LOG_INF("──────────────────────────────────────────────────");
	LOG_INF("SUMMARY  avg_freq=%d Hz  avg_err=%+d Hz  avg_ppm=%+d",
		avg_hz, avg_err_hz, avg_ppm);

	/* ── 5. Urteil ───────────────────────────────────────────────────── */
	/* XTAL: < ±500 ppm bei Raumtemperatur
	 * RC nach Kalibrierung: typisch ±500 ppm; unkalibriert bis ±2000 ppm
	 * Wir nutzen ±300 ppm als Grenze XTAL / RC                          */
	bool looks_like_xtal = (avg_ppm > -300 && avg_ppm < 300);
	bool running_rc      = (src_run == NRF_CLOCK_LFCLK_RC);

	LOG_INF("──────────────────────────────────────────────────");
	if (!running_rc && looks_like_xtal) {
		LOG_INF("URTEIL: XTAL — 32.768 kHz Quarz ist bestückt und läuft.");
	} else if (running_rc && !looks_like_xtal) {
		LOG_INF("URTEIL: RC — kein Quarz aktiv (ppm-Abweichung bestätigt RC).");
	} else if (running_rc && looks_like_xtal) {
		LOG_INF("URTEIL: RC (kalibriert oder sehr genau).");
		LOG_INF("        Mit CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y neu bauen,");
		LOG_INF("        um Quarz-Bestückung zu prüfen.");
	} else {
		LOG_INF("URTEIL: XTAL konfiguriert, aber ppm ausserhalb Toleranz — Quarz prüfen.");
	}
	LOG_INF("──────────────────────────────────────────────────");

	/* Endlos-Idle damit der serielle Port offen bleibt */
	while (true) {
		k_sleep(K_SECONDS(10));
		LOG_INF("(idle — Board resetten um neu zu messen)");
	}

	return 0;
}

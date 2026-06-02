# 098 — 32 kHz Clock Source Diagnostic

Bestimmt, ob das Seeed XIAO nRF52840 / nRF52840 Sense einen echten
32.768 kHz Quarz (LFXO) bestückt hat oder ausschließlich den internen
RC-Oszillator (LFRC) nutzt.

## Hintergrund

Das XIAO-nRF52840-Sense-Schaltbild zeigt einen 32.768-kHz-Quarz-Footprint,
aber viele Serienboards werden ohne Bestückung geliefert.  Ohne Quarz wartet
`CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y` beim Boot ewig auf `LFCLKSTARTED`
— das Board erscheint tot.

Dieses Sample liefert eine eindeutige Antwort:

1. **Register-Auslese** — `CLOCK.LFCLKSTAT` meldet die tatsächlich laufende
   LFCLK-Quelle (RC / XTAL) über die Nordic-HAL.
2. **Frequenzmessung** — 5 × 1 s Messfenster gaten RTC2 (LFCLK, PRESCALER=0)
   gegen ARM SysTick (64 MHz HFXO-Referenz).  RC vor Kalibrierung driftet
   ±500–2000 ppm; ein Quarz bleibt bei Raumtemperatur innerhalb ±50 ppm.
3. **Urteil** — wird auf der seriellen Konsole ausgegeben.

## Zwischen RC und XTAL wechseln

`boards/xiao_ble_sense.conf` editieren und die beiden Clock-Source-Zeilen
tauschen:

**RC-Oszillator** (immer sicher — funktioniert auch ohne Quarz):

```kconfig
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_500PPM=y
```

**XTAL-Oszillator** (nur wenn ein 32.768-kHz-Quarz bestückt ist):

```kconfig
CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_20PPM=y
```

> ⚠️ **Vorsicht:** Fehlt der Quarz, hängt das Board mit XTAL-Konfig beim Boot.
> Wiederherstellen: RC-Konfiguration setzen, doppelten RESET drücken und mit
> `make 098-serial-flash` neu flashen.

## Build & Flash

```sh
# Konfig anpassen (siehe oben), dann bauen und flashen:
make 098-build  BOARD=xiao_ble_sense
make 098-serial-flash BOARD=xiao_ble_sense SERIAL_PORT=/dev/ttyACM3
```

Serielle Ausgabe lesen (nach dem Flash, Gerät im normalen Modus):

```sh
minicom -b 115200 -D /dev/ttyACM3 -o
```

## Beispielausgabe

### RC-Oszillator (kein Quarz bestückt)

```log
*** Booting nRF Connect SDK v3.5.99-ncs1 ***
[00:00:00.501] <inf> test_32khz: === 32 kHz Clock Source Diagnostic ===
[00:00:00.501] <inf> test_32khz: Starte Messung in 3 s - bitte Terminal verbinden ...
[00:00:01.502] <inf> test_32khz: ... 2 ...
[00:00:02.503] <inf> test_32khz: ... 1 ...
[00:00:03.503] <inf> test_32khz: HFCLK Referenz: HFXO (±50 ppm)
[00:00:03.504] <inf> test_32khz: LFCLK source requested : RC
[00:00:03.604] <inf> test_32khz: LFCLK source running   : RC
[00:00:03.605] <inf> test_32khz: Messe LFCLK-Frequenz (SysTick @ 64 MHz vs RTC2-Gate) ...
[00:00:03.605] <inf> test_32khz: 5 x 32768 LFCLK-Ticks Fenster:
[00:00:04.606] <inf> test_32khz:   Fenster 1: 64140510 Zyklen  eff=32696 Hz  err=-72 Hz  ppm=-2197
[00:00:05.607] <inf> test_32khz:   Fenster 2: 64141318 Zyklen  eff=32695 Hz  err=-73 Hz  ppm=-2227
[00:00:06.608] <inf> test_32khz:   Fenster 3: 64141858 Zyklen  eff=32695 Hz  err=-73 Hz  ppm=-2227
[00:00:07.608] <inf> test_32khz:   Fenster 4: 64140386 Zyklen  eff=32696 Hz  err=-72 Hz  ppm=-2197
[00:00:08.609] <inf> test_32khz:   Fenster 5: 64139402 Zyklen  eff=32696 Hz  err=-72 Hz  ppm=-2197
[00:00:08.610] <inf> test_32khz: ──────────────────────────────────────────────────
[00:00:08.611] <inf> test_32khz: SUMMARY  avg_freq=32696 Hz  avg_err=-72 Hz  avg_ppm=-2209
[00:00:08.612] <inf> test_32khz: ──────────────────────────────────────────────────
[00:00:08.614] <inf> test_32khz: URTEIL: RC — kein Quarz aktiv (ppm-Abweichung bestätigt RC).
[00:00:08.615] <inf> test_32khz: ──────────────────────────────────────────────────
```

### XTAL-Oszillator (Quarz bestückt)

```log
*** Booting nRF Connect SDK v3.5.99-ncs1 ***
[00:00:00.292] <inf> test_32khz: === 32 kHz Clock Source Diagnostic ===
[00:00:00.293] <inf> test_32khz: Starte Messung in 3 s - bitte Terminal verbinden ...
[00:00:01.294] <inf> test_32khz: ... 2 ...
[00:00:02.294] <inf> test_32khz: ... 1 ...
[00:00:03.295] <inf> test_32khz: HFCLK Referenz: HFXO (±50 ppm)
[00:00:03.295] <inf> test_32khz: LFCLK source requested : XTAL
[00:00:03.396] <inf> test_32khz: LFCLK source running   : XTAL
[00:00:03.397] <inf> test_32khz: Messe LFCLK-Frequenz (SysTick @ 64 MHz vs RTC2-Gate) ...
[00:00:03.398] <inf> test_32khz: 5 x 32768 LFCLK-Ticks Fenster:
[00:00:04.398] <inf> test_32khz:   Fenster 1: 63997942 Zyklen  eff=32769 Hz  err=+1 Hz  ppm=+30
[00:00:05.399] <inf> test_32khz:   Fenster 2: 63997946 Zyklen  eff=32769 Hz  err=+1 Hz  ppm=+30
[00:00:06.400] <inf> test_32khz:   Fenster 3: 63997962 Zyklen  eff=32769 Hz  err=+1 Hz  ppm=+30
[00:00:07.401] <inf> test_32khz:   Fenster 4: 63997966 Zyklen  eff=32769 Hz  err=+1 Hz  ppm=+30
[00:00:08.401] <inf> test_32khz:   Fenster 5: 63997978 Zyklen  eff=32769 Hz  err=+1 Hz  ppm=+30
[00:00:08.402] <inf> test_32khz: ──────────────────────────────────────────────────
[00:00:08.404] <inf> test_32khz: SUMMARY  avg_freq=32769 Hz  avg_err=+1 Hz  avg_ppm=+30
[00:00:08.405] <inf> test_32khz: ──────────────────────────────────────────────────
[00:00:08.406] <inf> test_32khz: URTEIL: XTAL — 32.768 kHz Quarz ist bestückt und läuft.
[00:00:08.407] <inf> test_32khz: ──────────────────────────────────────────────────
```

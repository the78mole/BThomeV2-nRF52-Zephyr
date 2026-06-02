# 200 — BThome IMU

Liest den eingebauten 6-Achsen-IMU **LSM6DS3TR-C** des Seeed XIAO nRF52840
Sense und advertised Beschleunigung (X/Y/Z) und Gyroskop-Betrag |ω| alle 5 s
als BThome-V2-Payload. Optimiert für Batteriebetrieb mit `bt_disable()` /
`pm_device_action_run()` pro Zyklus.

## BThome-Objekte

| Obj-ID | Property          | Datentyp | Faktor    | Einheit | Anzahl |
|--------|-------------------|----------|-----------|---------|--------|
| `0x63` | acceleration axis | sint32   | 0.000001  | m/s²    | 3 (X/Y/Z) |
| `0x52` | gyroscope         | uint16   | 0.001     | °/s     | 1 (|ω| Betrag) |
| `0x00` | packet id         | uint8    | —         | —       | 1      |

### Encoding-Beispiel

`0x63` Beschleunigung -3.123456 m/s²:
```
63 00 57 D0 FF
│  └─────────── sint32 LE: 0xFFD05700 = -3 123 456 → × 0.000001 = -3.123456 m/s²
└─ Object ID
```

`0x52` Gyroskop-Betrag 22.151 °/s:
```
52 87 56
│  └──── uint16 LE: 0x5687 = 22151 → × 0.001 = 22.151 °/s
└─ Object ID
```

## Nutzlast-Budget

```
BTHOME_V2_SVC_DATA_MAX_LEN = 26 B
  davon BThome-Header (UUID×2 + devinfo) = 3 B
  verfügbar für Objekte                  = 23 B

pkt_id   0x00  1+1 =  2 B
accel_x  0x63  1+4 =  5 B
accel_y  0x63  1+4 =  5 B
accel_z  0x63  1+4 =  5 B
gyro_mag 0x52  1+2 =  3 B
                      ────
                     20 B ✓  (3 B Reserve)
```

Der Gerätename `MAKE-IMU` befindet sich im **Scan Response** (nicht im
Advertisement), damit die 23 Byte vollständig für die Messdaten verfügbar
bleiben.

## Hardware

| Merkmal | Details |
|---------|---------|
| Board | Seeed XIAO nRF52840 Sense |
| IMU | LSM6DS3TR-C (Zephyr-Treiber: `st,lsm6dsl`) |
| I2C-Adresse | `0x6A` |
| IMU-Stromversorgung | GPIO P1.08 (Regulator `LSM6DS3TR_C_EN`, boot-on) |
| Active Indicator | GPIO P0.05 (D5) — HIGH während BLE-ADV, LOW im Schlaf |

## Sensorwert-Konvertierung

**Beschleunigung** (Zephyr `sensor_value` in m/s² → BThome `sint32`):
```c
int32_t raw = (int32_t)((int64_t)v->val1 * 1000000 + v->val2);
// Zephyr val2 ist bereits der mikrofraktionale Anteil → direkte Addition
```

**Gyroskop** (Zephyr `sensor_value` in rad/s → BThome `uint16` in mdeg/s):
```c
int64_t uras = (int64_t)v->val1 * 1000000 + v->val2;   // µrad/s
int64_t mdeg = uras * 180000LL / 3141593LL;             // mdeg/s
// Klammerung auf [0, 65535]
```

**Gyroskop-Betrag** (|ω| = Euklidische Norm aller 3 Achsen):
```c
uint64_t sq = (uint64_t)gx*gx + (uint64_t)gy*gy + (uint64_t)gz*gz;
uint32_t mag = (uint32_t)sqrtf((float)sq);
// Semantisch korrekt: repräsentiert die gesamte Rotationsgeschwindigkeit
```

## Low-Power-Architektur

Pro 5-Sekunden-Zyklus:

```
+----------------------------------------------------------------------+
|  [1 s ADV-Fenster]          [4 s Schlaf (WFE/WFI)]                  |
|  BLE+IMU aktiv              BLE+IMU aus                              |
|  bt_enable() . fetch . adv  bt_disable() . PM_SUSPEND . k_sleep     |
|  D5 = HIGH                  D5 = LOW                                 |
+----------------------------------------------------------------------+
```

Wichtig: `CONFIG_BT_UNINIT_MPSL_ON_DISABLE` ist **nicht** gesetzt. Mit diesem
Flag würde der MPSL-Clock-Arbiter entfernt, die RC-Kalibrierung hätte keinen
Arbiter mehr → HFCLK bleibt dauerhaft aktiv → ~400 µA Floor. Ohne das Flag:
`bt_disable()` gibt HFCLK frei, MPSL verwaltet die Kalibrierung korrekt.

## Gemessene Stromaufnahme (PPK2, XIAO nRF52840 Sense)

> Messdatei: `data/200-ppk2-20260602T041052.ppk2` (60 s, 100 kS/s)

| Phase | Dauer | Avg | Peak |
|-------|-------|-----|------|
| Boot | 3.3 s | — | 76.5 mA |
| ADV-Fenster (D5=HIGH) | ~1.0 s / Zyklus | **458 µA** | 15.7 mA |
| Schlaf (D5=LOW) | ~4.0 s / Zyklus | **~15.7 µA** | — |
| **Steady-State-Mittel** | | **107.7 µA** | |
| Gesamt-Mittel (inkl. Boot) | 60 s | 229.8 µA | |

### Batterielaufzeit (aus Steady-State 107.7 µA)

| Batterie | Kapazität | Laufzeit |
|----------|-----------|----------|
| CR2032 1× 3.0 V | 230 mAh | **~89 Tage** (2135 h) |
| 2× AA 3.0 V | 2500 mAh | **~2.6 Jahre** (967 Tage) |
| 2× AAA 3.0 V | 1200 mAh | **~1.3 Jahre** (464 Tage) |

## Build & Flash

```sh
make 200-build         BOARD=xiao_ble_sense
make 200-serial-flash  BOARD=xiao_ble_sense SERIAL_PORT=/dev/ttyACM3
```

Serielle Ausgabe (nur im Entwicklungsmodus — für Batteriebetrieb
ist `CONFIG_LOG=n` in `boards/xiao_ble_sense.conf` gesetzt):
```sh
minicom -b 115200 -D /dev/ttyACM3 -o
```

## Empfang mit bthome-logger

```sh
uv tool install bthome-logger
bthome-logger -f "MAKE-IMU"
```

## PPK2-Stromanalyse

```sh
uv run scripts/ppk_analysis.py data/200-ppk2-20260602T041052.ppk2 --per-second
```

Die D0-Leitung des PPK2 ist mit P0.05 (XIAO D5, Active Indicator) verbunden.
Die Boot-Phase wird automatisch erkannt und aus dem Steady-State-Mittelwert
ausgeschlossen.

## Typische Serielle Ausgabe

```log
*** Booting nRF Connect SDK v3.5.99-ncs1 ***
[00:00:00.501] <inf> bthome_imu: === BThome IMU Sample ===
[00:00:00.503] <inf> bthome_imu: IMU bereit: lsm6ds3tr-c@6a
[00:00:00.505] <inf> bthome_imu: Bluetooth bereit
[00:00:00.506] <inf> bthome_imu: Accel:    X=-12345 Y=3456 Z=9812000 µm/s²
[00:00:00.507] <inf> bthome_imu: Gyro XYZ: X=0 Y=12 Z=523 mdeg/s
[00:00:00.508] <inf> bthome_imu: Gyro |ω|: 523 mdeg/s  (0.523 °/s)  -> BThome
[00:00:00.509] <inf> bthome_imu: ADV pkt=0  ax=-12345 ay=3456 az=9812000 µm/s²  gz=523 mdeg/s
-- [BLE off + IMU suspended, 4 s tickless sleep] --
[00:00:05.xxx] <inf> bthome_imu: Accel:    X=...
```

## Erweiterungsmöglichkeiten

- **IMU-Temperatur:** Zephyr-Kanal `SENSOR_CHAN_DIE_TEMP`, BThome-Objekt `0x02` (sint16, 0.01 °C)
- **Längere Schlafzeit:** `SLEEP_MS` erhöhen (z. B. 25 s) → noch niedrigerer Mittelstrom
- **System OFF statt WFI:** Architektur aus Sample 099 übernehmen → ~11 µA Schlafstrom möglich

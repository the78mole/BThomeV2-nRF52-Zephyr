# 200 — BThome IMU

Liest den eingebauten 6-Achsen-IMU **LSM6DS3TR-C** des Seeed XIAO nRF52840
Sense und advertised Beschleunigung (X/Y/Z) und Gyroskop (Z-Achse) alle 5 s
als BThome-V2-Payload.

## BThome-Objekte

| Obj-ID | Property          | Datentyp | Faktor    | Einheit | Anzahl |
|--------|-------------------|----------|-----------|---------|--------|
| `0x63` | acceleration axis | sint32   | 0.000001  | m/s²    | 3 (X/Y/Z) |
| `0x52` | gyroscope         | uint16   | 0.001     | °/s     | 1 (Z-Achse) |
| `0x00` | packet id         | uint8    | —         | —       | 1      |

### Encoding-Beispiel

`0x63` Beschleunigung -3.123456 m/s²:
```
63 00 57 D0 FF
│  └──────────── sint32 LE: 0xFFD05700 = -3 123 456 → × 0.000001 = -3.123456 m/s²
└─ Object ID
```

`0x52` Gyroskop 22.151 °/s:
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
gyro_z   0x52  1+2 =  3 B
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

## Build & Flash

```sh
make 200-build         BOARD=xiao_ble_sense
make 200-serial-flash  BOARD=xiao_ble_sense SERIAL_PORT=/dev/ttyACM3
```

Serielle Ausgabe:
```sh
minicom -b 115200 -D /dev/ttyACM3 -o
```

## Empfang mit bthome-logger

```sh
uv tool install bthome-logger
bthome-logger -f "MAKE-IMU"
```

## Typische Serielle Ausgabe

```log
*** Booting nRF Connect SDK v3.5.99-ncs1 ***
[00:00:00.501] <inf> bthome_imu: === BThome IMU Sample ===
[00:00:00.503] <inf> bthome_imu: IMU bereit: lsm6ds3tr-c@6a
[00:00:00.505] <inf> bthome_imu: Bluetooth bereit
[00:00:00.506] <inf> bthome_imu: Accel: X=-12345 Y=3456 Z=9812000 µm/s²
[00:00:00.507] <inf> bthome_imu: Gyro Z: 523 mdeg/s  (0.523 °/s)
[00:00:00.508] <inf> bthome_imu: ADV pkt=0  ax=-12345 ay=3456 az=9812000 µm/s²  gz=523 mdeg/s
[00:00:01.510] <inf> bthome_imu: Accel: X=-12201 Y=3389 Z=9813120 µm/s²
...
```

## Erweiterungsmöglichkeiten

- Alle 3 Gyro-Achsen: pkt_id weglassen (spart 2 B), gyro_y + gyro_x hinzufügen → 24 B (1 B über Limit!) → stattdessen pkt_id und eine Gyro-Achse opfern
- Temperatur des IMU: Zephyr-Kanal `SENSOR_CHAN_DIE_TEMP`, BThome-Objekt `0x02` (sint16, 0.01 °C)
- Energiesparmodus: `bt_disable()` nach ADV-Fenster + `k_event_wait()` statt `k_sleep()` → gleiche Architektur wie Sample 100

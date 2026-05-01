---
name: bthome-v2-nrf52
description: >-
  Domain-knowledge skill covering the BThome V2 protocol and its implementation
  on Nordic nRF52/nRF53 hardware using the nRF Connect SDK (Zephyr RTOS).
  Includes object IDs, encoding rules, advertising format, and the bthome_v2
  Zephyr library contained in this repository.
user-invocable: true
---

# BThome V2 — Domain Knowledge (nRF Connect SDK / Zephyr)

## 1. Was ist BThome V2?

**BThome V2** ist ein offenes, kompaktes Binär-Format für BLE-Advertising, das
Sensordaten effizient überträgt und nativ von Home Assistant erkannt wird.

Kernmerkmale:
- **Kein Pairing** – Daten werden als nicht-verbindbares Broadcast-Advertising verteilt
- **16-Bit Service UUID `0xFCD2`** im Service-Data-AD-Feld
- **Kompakt** – bis zu 23 Byte Nutzdaten im Standard-Advertising-Paket
- **Sortierte Objekte** – Messungen müssen aufsteigend nach Object-ID sortiert sein
- **Optionale AES-CCM-Verschlüsselung** (Bind Key + Packet Counter + MIC)
- **Versionierung** – Bits 6-7 des Device-Info-Bytes kodieren die Version (= 0x40 für V2)

Spezifikation: <https://bthome.io/format/>
Arduino-Referenz: <https://github.com/the78mole/bthomev2>

---

## 2. Advertising-Paket-Struktur

Ein vollständiges BLE-Advertising-Paket mit BThome V2 sieht so aus:

```
┌────────────────────────────────────────────────────────────────────────┐
│  Flags AD  (3 Byte)                                                    │
│  02 01 06                                                              │
│  ↑  ↑  ↑                                                               │
│  │  │  └─ LE General Discoverable | BR/EDR not supported              │
│  │  └──── AD Type: Flags (0x01)                                        │
│  └─────── Länge: 2                                                     │
├────────────────────────────────────────────────────────────────────────┤
│  Service Data AD  (3 + N Byte)                                         │
│  LL 16 D2 FC 40 [OBJ_ID DATA] [OBJ_ID DATA] …                         │
│  ↑  ↑  ↑──↑  ↑                                                        │
│  │  │  │  │  └─ Device Info Byte (0x40 = Version 2, kein Encrypt)     │
│  │  │  └──┘─── UUID 0xFCD2, little-endian: D2 FC                      │
│  │  └──────── AD Type: Service Data 16-Bit UUID (0x16)                │
│  └─────────── Länge: 2 + 1 + N                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### Device-Info-Byte

| Bit | Bedeutung | Wert |
|-----|-----------|------|
| 0 | Encryption | 1 = verschlüsselt |
| 1 | reserviert | 0 |
| 2 | Trigger-based | 1 = nicht-periodisch |
| 3-5 | reserviert | 0 |
| 6-7 | BTHome Version | 10 = Version 2 → **0x40** |

Beispiele: `0x40` = V2 unverschlüsselt, `0x41` = V2 verschlüsselt, `0x44` = V2 trigger-based.

---

## 3. Object-ID-Tabelle (vollständig, laut Spezifikation)

### Numerische Sensoren

| Object ID | Name | Datentyp | Faktor | Einheit |
|-----------|------|----------|--------|---------|
| `0x00` | Packet ID | uint8 | 1 | — |
| `0x01` | Battery | uint8 | 1 | % |
| `0x02` | Temperature | **sint16** | **0.01** | °C |
| `0x03` | Humidity | uint16 | 0.01 | % |
| `0x04` | Pressure | uint24 | 0.01 | hPa |
| `0x05` | Illuminance | uint24 | 0.01 | lx |
| `0x06` | Mass (kg) | uint16 | 0.01 | kg |
| `0x07` | Mass (lb) | uint16 | 0.01 | lb |
| `0x08` | Dew point | sint16 | 0.01 | °C |
| `0x09` | Count | uint8 | 1 | — |
| `0x0A` | Energy | uint24 | 0.001 | kWh |
| `0x0B` | Power | uint24 | 0.01 | W |
| `0x0C` | Voltage | uint16 | 0.001 | V |
| `0x0D` | PM2.5 | uint16 | 1 | µg/m³ |
| `0x0E` | PM10 | uint16 | 1 | µg/m³ |
| `0x0F` | Generic Boolean | uint8 | 1 | 0/1 |
| `0x10` | Power (binary) | uint8 | 1 | 0/1 |
| `0x11` | Opening (binary) | uint8 | 1 | 0/1 |
| `0x12` | CO₂ | uint16 | 1 | ppm |
| `0x13` | TVOC | uint16 | 1 | µg/m³ |
| `0x14` | Moisture | uint16 | 0.01 | % |

### Binäre Sensoren (alle uint8, 0 = inaktiv, 1 = aktiv)

| Object ID | Name | Object ID | Name |
|-----------|------|-----------|------|
| `0x15` | Battery Low | `0x25` | Presence |
| `0x16` | Battery Charging | `0x26` | Problem |
| `0x17` | CO (Kohlenmonoxid) | `0x27` | Running |
| `0x18` | Cold | `0x28` | Safety |
| `0x19` | Connectivity | `0x29` | Smoke |
| `0x1A` | Door | `0x2A` | Sound |
| `0x1B` | Garage Door | `0x2B` | Tamper |
| `0x1C` | Gas | `0x2C` | Vibration |
| `0x1D` | Heat | `0x2D` | Window |
| `0x1E` | Light | `0x2E` | Humidity (uint8, 1 %) |
| `0x1F` | Lock | `0x2F` | Moisture (uint8, 1 %) |
| `0x20` | Moisture (binary) | | |
| `0x21` | **Motion** | | |
| `0x22` | Moving | | |
| `0x23` | Occupancy | | |
| `0x24` | Plug | | |

> ⚠️ **Achtung:** Der `WINDOW`-Object-ID ist `0x2D` (nicht `0x2F`). IDs `0x2E` und `0x2F`
> sind numerische Feuchtigkeitswerte. Eine häufige Fehlerquelle in älteren Bibliotheken!

### Events

| Object ID | Name | Datentyp |
|-----------|------|----------|
| `0x3A` | Button | uint8 (Event-Code, s.u.) |
| `0x3C` | Dimmer | uint16 (low=Richtung, high=Schritte) |

**Button-Event-Codes:**

| Code | Bedeutung |
|------|-----------|
| `0x00` | None |
| `0x01` | Press |
| `0x02` | Double Press |
| `0x03` | Triple Press |
| `0x04` | Long Press |
| `0x05` | Long Double Press |
| `0x06` | Long Triple Press |
| `0x08` | Hold Press |

### Erweiterte Sensoren (0x3D …)

| Object ID | Name | Datentyp | Faktor | Einheit |
|-----------|------|----------|--------|---------|
| `0x3D` | Count | uint16 | 1 | — |
| `0x3E` | Count | uint32 | 1 | — |
| `0x3F` | Rotation | sint16 | 0.1 | ° |
| `0x40` | Distance | uint16 | 1 | mm |
| `0x41` | Distance | uint16 | 0.1 | m |
| `0x42` | Duration | uint24 | 0.001 | s |
| `0x43` | Current | uint16 | 0.001 | A |
| `0x44` | Speed | uint16 | 0.01 | m/s |
| **`0x45`** | **Temperature** | **sint16** | **0.1** | **°C** |
| `0x46` | UV Index | uint8 | 0.1 | — |
| `0x47` | Volume | uint16 | 0.1 | L |
| `0x48` | Volume | uint16 | 1 | L |
| `0x49` | Volume Flow Rate | uint16 | 0.001 | m³/hr |
| `0x4A` | Voltage | uint16 | 0.1 | V |
| `0x4B` | Gas | uint24 | 0.001 | m³ |
| `0x4C` | Gas | uint32 | 0.001 | m³ |
| `0x4D` | Energy | uint32 | 0.001 | kWh |
| `0x4E` | Volume | uint32 | 0.001 | L |
| `0x4F` | Water | uint32 | 0.001 | L |
| `0x50` | Timestamp | uint32 | 1 | Unix-s |
| **`0x51`** | **Acceleration** | **uint16** | **0.001** | **m/s²** |
| **`0x52`** | **Gyroscope** | **uint16** | **0.001** | **°/s** |
| `0x53` | Text | var (1-Byte-Len-Prefix) | — | — |
| `0x54` | Raw | var (1-Byte-Len-Prefix) | — | — |
| `0x55` | Volume Storage | uint32 | 0.001 | L |
| `0x56` | Conductivity | uint16 | 1 | µS/cm |
| `0x57` | Temperature | sint8 | 1 | °C |
| `0x58` | Temperature | sint8 | 0.35 | °C |
| `0x59` | Count | sint8 | 1 | — |
| `0x5A` | Count | sint16 | 1 | — |
| `0x5B` | Count | sint32 | 1 | — |
| `0x5C` | Power | sint32 | 0.01 | W |
| `0x5D` | Current | sint16 | 0.001 | A |
| `0x5E` | Direction | uint16 | 0.01 | ° |
| `0x5F` | Precipitation | uint16 | 0.1 | mm |
| `0x60` | Channel | uint8 | 1 | — |

> **Schlüsselunterschied `0x02` vs `0x45`:**
> - `0x02` Temperature: sint16, Faktor **0.01** → Bereich ±327.67 °C, Auflösung 0.01 °C
> - `0x45` Temperature: sint16, Faktor **0.1** → Bereich ±3276.7 °C, Auflösung 0.1 °C
>
> Für typische Umgebungstemperaturen ist `0x02` die präzisere Wahl. `0x45` wird in
> bthome.io-Dokumentation explizit als zweite Temperature-Variante gelistet.

---

## 4. Encoding-Regeln

1. **Sortierung:** Alle Objekte MÜSSEN aufsteigend nach Object-ID sortiert sein.
2. **Little-Endian:** Alle Mehrbyted-Werte werden LE kodiert.
3. **Kein Trennzeichen** zwischen Objekten – nur `[OBJ_ID][Daten-Bytes…]`.
4. **Maximale Payload:** 23 Byte (bei Standard-ADV-Paket ohne extended advertising).

### Encoding-Beispiel: Temperatur 23.50 °C + Motion aktiv

```
Object 0x02 (Temperature 23.50 °C):
  Wert = 2350 (= 23.50 × 100)
  LE-sint16: 0x2E 0x09   (0x092E = 2350)

Object 0x21 (Motion = 1):
  uint8: 0x01

Service Data Payload (sortiert!):
  D2 FC        ← UUID 0xFCD2, LE
  40           ← Device Info: Version 2, kein Encrypt
  02 2E 09     ← Temperature: ID=0x02, Wert=2350
  21 01        ← Motion:      ID=0x21, aktiv
```

---

## 5. Die `bthome_v2`-Library (dieses Repository)

### Pfad: `lib/bthome_v2/`

```
lib/bthome_v2/
├── CMakeLists.txt
├── Kconfig                        CONFIG_BTHOME_V2
├── include/bthome_v2/
│   └── bthome_v2.h                Öffentliche API + alle Object-ID-Defines
└── src/
    └── bthome_v2.c                Implementierung
```

### Kconfig aktivieren

```kconfig
CONFIG_BTHOME_V2=y
```

### API-Überblick

```c
#include <bthome_v2/bthome_v2.h>

/* 1. Kontext initialisieren */
struct bthome_v2_ctx ctx;
bthome_v2_init(&ctx, /*encrypted=*/false, /*trigger_based=*/false);

/* 2. Messungen hinzufügen (Reihenfolge beliebig, wird beim Encode sortiert) */
bthome_v2_add_battery(&ctx, 85);               // 85 %
bthome_v2_add_temperature(&ctx, 2350);         // 23.50 °C (0x02, Faktor 0.01)
bthome_v2_add_temperature_01(&ctx, 235);       // 23.5  °C (0x45, Faktor 0.1)
bthome_v2_add_humidity(&ctx, 5500);            // 55.00 %
bthome_v2_add_pressure(&ctx, 101325);          // 1013.25 hPa
bthome_v2_add_illuminance(&ctx, 50000);        // 500.00 lx
bthome_v2_add_co2(&ctx, 450);                  // 450 ppm
bthome_v2_add_acceleration(&ctx, 9810);        // 9.810 m/s²
bthome_v2_add_gyroscope(&ctx, 1500);           // 1.500 °/s
bthome_v2_add_binary(&ctx, BTHOME_OBJ_MOTION, true);  // Motion detected
bthome_v2_add_button(&ctx, BTHOME_BTN_EVT_PRESS);

/* 3. Encodieren (sortiert nach OBJ_ID, befüllt ctx.svc_data) */
int len = bthome_v2_encode(&ctx);

/* 4. bt_data für Zephyr-BLE holen */
struct bt_data svc_data_entry;
bthome_v2_get_bt_data(&ctx, &svc_data_entry);

/* 5. Advertising-Array aufbauen */
struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    svc_data_entry,   // Service Data mit UUID + BThome-Payload
};
bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);

/* 6. Nächste Messung: Queue leeren, neue Werte, neu encodieren */
bthome_v2_clear(&ctx);
bthome_v2_add_temperature(&ctx, 2360);
bthome_v2_encode(&ctx);
bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
```

### Sensorwerte aus Zephyr-`sensor_value` umrechnen

```c
struct sensor_value temp_sv;
sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &temp_sv);

/* sensor_value → BThome-Einheit für 0x02 (Faktor 0.01 °C) */
int16_t temp_cdegc = (int16_t)(temp_sv.val1 * 100 +
                                temp_sv.val2 / 10000);
bthome_v2_add_temperature(&ctx, temp_cdegc);

/* Beschleunigung: Zephyr liefert m/s², BThome 0x51 = 0.001 m/s² */
struct sensor_value accel[3];
sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
/* Betragssumme (vereinfacht) */
uint16_t ax_mms2 = (uint16_t)(accel[0].val1 * 1000 +
                               accel[0].val2 / 1000);
bthome_v2_add_acceleration(&ctx, ax_mms2);
```

---

## 6. BLE Advertising für BThome V2 auf nRF52840

### Non-Connectable Advertising (empfohlen für Sensoren)

```c
static const struct bt_le_adv_param adv_param = {
    .options    = BT_LE_ADV_OPT_USE_IDENTITY,
    .interval_min = BT_GAP_ADV_SLOW_INT_MIN,  /* ~1 s */
    .interval_max = BT_GAP_ADV_SLOW_INT_MAX,
};
```

### Advertising-Daten aktualisieren (ohne Neustart)

```c
/* Messung aktualisieren, ohne advertising zu stoppen */
bthome_v2_clear(&ctx);
bthome_v2_add_temperature(&ctx, new_temp);
bthome_v2_encode(&ctx);
bthome_v2_get_bt_data(&ctx, &ad[1]);
bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
```

---

## 7. Overlays für I2C-Sensoren

### nRF52840-DK: MPU-6050 auf I2C0 (SDA=P0.26, SCL=P0.27)

```dts
/* boards/nrf52840dk_nrf52840.overlay */
&arduino_i2c {          /* alias für &i2c0 auf dem DK */
    mpu6050: mpu6050@68 {
        compatible = "invensense,mpu6050";
        reg = <0x68>;
        status = "okay";
    };
};

/ {
    aliases { imu = &mpu6050; };
};
```

### XIAO nRF52840 Sense: LSM6DS3 auf I2C0

```dts
/* boards/seeed_xiao_ble_sense.overlay */
&i2c0 {
    status = "okay";
    lsm6ds3: lsm6ds3@6a {
        compatible = "st,lsm6dsl";
        reg = <0x6a>;
        irq-gpios = <&gpio0 3 GPIO_ACTIVE_HIGH>;
        status = "okay";
    };
};

/ {
    aliases { imu = &lsm6ds3; };
};
```

### PIR-Sensor als GPIO

```dts
/ {
    pir_node: pir_sensor {
        compatible = "gpio-keys";
        pir0: pir_0 {
            gpios = <&gpio1 1 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>;
            label = "PIR Sensor";
        };
    };
    aliases { pir-sensor = &pir0; };
};
```

---

## 8. LFCLK-Konfiguration für XIAO nRF52840

Das Seeed XIAO nRF52840 hat **kein externes 32,768-kHz-Quarz**. Ohne korrekte
LFCLK-Konfiguration hängt der BLE-Stack beim Initialisieren.

```kconfig
# boards/seeed_xiao_ble.conf  oder  boards/seeed_xiao_ble_sense.conf
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_ACCURACY_500=y
```

> Der reguläre Board-Support in NCS (≥ v2.4) setzt diese Werte bereits korrekt.
> Bei älteren Versionen oder Custom-Boards müssen sie explizit gesetzt werden.

---

## 9. Vollständiges prj.conf für bthome_full_node

```kconfig
# Bluetooth
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="BThome-Sensor"

# BThome V2 Library
CONFIG_BTHOME_V2=y

# Sensor-Framework
CONFIG_SENSOR=y
CONFIG_I2C=y

# Interne Temperatur (nRF52-Die-Sensor)
CONFIG_TEMP_NRF5=y

# IMU (MPU-6050 oder LSM6DSL je nach Board)
CONFIG_MPU6050=y
# CONFIG_LSM6DSL=y

# Logging
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3

# Power Management (optional, für Energie-Optimierung)
CONFIG_PM=y
CONFIG_PM_DEVICE=y
```

---

## 10. Home Assistant Integration

Damit Home Assistant BThome-V2-Geräte automatisch erkennt:

1. **Bluetooth-Integration** aktivieren (Einstellungen → Geräte & Dienste)
2. **BTHome-Integration** automatisch erkannt, sobald ein Gerät sendet
3. Gerätename = BLE-Gerätename (im Advertising-Paket)

Beispiel-Entitäten, die Home Assistant aus `bthome_full_node` erstellt:
- `sensor.bthome_sensor_temperature` (aus 0x02)
- `binary_sensor.bthome_sensor_motion` (aus 0x21)
- `sensor.bthome_sensor_battery` (aus 0x01)

---

## 11. Testen mit bthome-logger

```bash
# Python-Tool installieren (gleiche Version wie Library)
uv tool install bthome-logger

# BLE-Advertisements scannen und dekodieren
bthome-logger

# Nur eigenes Gerät filtern
bthome-logger -f "BThome-Sensor"
```

Beispielausgabe:
```
BThome-Sensor (AA:BB:CC:DD:EE:FF) RSSI: -65 dBm
  Battery (0x01):        85 %
  Temperature (0x02):    23.50 °C
  Humidity (0x03):       55.00 %
  Motion (0x21):         detected
  Acceleration (0x51):   9.810 m/s²
```

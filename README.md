# BThomeV2-nRF52-Zephyr

A modular Zephyr / nRF Connect SDK firmware repository that encodes and
broadcasts sensor data using the **BThome V2** protocol over BLE.

Supported hardware: **nRF52840-DK**, **Seeed XIAO nRF52840**, **Seeed XIAO nRF52840 Sense**

Specification: <https://bthome.io/>  
Reference Arduino library: <https://github.com/the78mole/bthomev2>

---

## Repository Structure

```
.
├── west.yml                        # NCS SDK manifest (sdk-nrf v2.6.0)
├── CMakeLists.txt                  # Zephyr module root
├── Kconfig                         # Zephyr module Kconfig root
├── zephyr/module.yml               # Zephyr module registration
│
├── lib/bthome_v2/                  # Pure-C BThome V2 encoder library
│   ├── include/bthome_v2/
│   │   └── bthome_v2.h             # All object IDs + clean Zephyr API
│   └── src/
│       └── bthome_v2.c             # Encoder (sorts by OBJ_ID, fills bt_data)
│
└── samples/
    ├── 000_blinky/                 # LED blink: 100 ms ON / 900 ms OFF
    └── 010_bthome-tut1/            # Die-temp + 6-axis IMU + PIR → BThome V2 ADV
```

---

## Quick Start

### Prerequisites

```bash
# Install west and the nRF Connect SDK toolchain
pip install west
west init -l .
west update
```

## Samples

| # | Verzeichnis | Beschreibung |
|---|-------------|--------------|
| 000 | [`samples/000_blinky`](samples/000_blinky) | Minimales Blinky: LED1 blinkt 100 ms / 900 ms — Smoke-Test für Toolchain und Flash-Workflow |
| 010 | [`samples/010_bthome-tut1`](samples/010_bthome-tut1) | Vollständiger BThome-V2-Node: interne Die-Temperatur, 6-Achsen-IMU (MPU-6050 / LSM6DS3) und PIR-Bewegungssensor im BLE-Advertisement |
| 020 | [`samples/020_bthome_tut2`](samples/020_bthome_tut2) | Erweiterung von 010 um Power-Management: Sensoren und BLE werden nur bei Bewegung aktiv, ansonsten Tiefschlaf |
---

### Build & Flash

```bash
# Blinky – nRF52840-DK
west build -b nrf52840dk_nrf52840 samples/000_blinky && west flash

# Blinky – Seeed XIAO nRF52840
west build -b seeed_xiao_ble_nrf52840 samples/000_blinky && west flash

# Full BThome node – nRF52840-DK (MPU-6050 on Arduino I2C header)
west build -b nrf52840dk_nrf52840 samples/010_bthome-tut1 && west flash

# Full BThome node – Seeed XIAO nRF52840 Sense (LSM6DS3 built-in)
west build -b seeed_xiao_ble_nrf52840 samples/010_bthome-tut1 && west flash
```

### Verify BThome V2 Advertisements

```bash
uv tool install bthome-logger
bthome-logger -f "BThome-Sensor"
```

---

## `lib/bthome_v2` Library

Enable via Kconfig:

```kconfig
CONFIG_BTHOME_V2=y
```

Minimal usage:

```c
#include <bthome_v2/bthome_v2.h>

struct bthome_v2_ctx bthome;
struct bt_data ad[2];

bthome_v2_init(&bthome, false, false);
bthome_v2_add_temperature(&bthome, 2350);        // 23.50 °C  (OBJ 0x02)
bthome_v2_add_binary(&bthome, BTHOME_OBJ_MOTION, true);
bthome_v2_encode(&bthome);                       // sorts by OBJ_ID

ad[0] = (struct bt_data) BT_DATA_BYTES(BT_DATA_FLAGS,
           BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);
bthome_v2_get_bt_data(&bthome, &ad[1]);
bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
```

### Key Object IDs

| Macro | ID | Type | Factor | Unit |
|-------|----|------|--------|------|
| `BTHOME_OBJ_TEMPERATURE` | `0x02` | sint16 | 0.01 | °C |
| `BTHOME_OBJ_TEMPERATURE_01` | `0x45` | sint16 | 0.1 | °C |
| `BTHOME_OBJ_HUMIDITY` | `0x03` | uint16 | 0.01 | % |
| `BTHOME_OBJ_PRESSURE` | `0x04` | uint24 | 0.01 | hPa |
| `BTHOME_OBJ_MOTION` | `0x21` | uint8 | — | 0/1 |
| `BTHOME_OBJ_ACCELERATION` | `0x51` | uint16 | 0.001 | m/s² |
| `BTHOME_OBJ_GYROSCOPE` | `0x52` | uint16 | 0.001 | °/s |

See `lib/bthome_v2/include/bthome_v2/bthome_v2.h` for the complete list.

---

## XIAO nRF52840 — Important Notes

The XIAO has **no external 32.768 kHz crystal**. Add this to your board `.conf`:

```kconfig
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_ACCURACY_500=y
```

(Already included in `samples/bthome_full_node/boards/seeed_xiao_ble.conf`)

---

## License

MIT — see [LICENSE](LICENSE)

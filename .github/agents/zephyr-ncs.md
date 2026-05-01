---
name: zephyr-ncs
description: >-
  Domain-knowledge skill covering the Zephyr RTOS and the nRF Connect SDK (NCS).
  Explains project structure, build system, device tree, Kconfig, BLE stack,
  sensor drivers, and best practices for Nordic nRF52/nRF53/nRF91 targets.
  Documentation language: German (target audience: German-speaking developers).
user-invocable: true
---

# Zephyr RTOS & nRF Connect SDK — Domain Knowledge

> **Language note:** This skill is written in German to match the project's
> primary audience. Use this skill whenever working on Zephyr / NCS firmware
> in this repository.


## 1. Überblick

**Zephyr RTOS** ist ein quelloffenes, skalierbares Echtzeit-Betriebssystem (Apache-2.0),
das von der Linux Foundation gehostet wird. Es zielt auf ressourcenbeschränkte Embedded-
Systeme ab (µC ab Cortex-M0 aufwärts) und bietet:

- Kooperatives und präemptives Scheduling (FIFO, Round-Robin, Priority)
- POSIX-Thread-API und Kernel-Primitives (Semaphore, Mutex, MessageQueue, k_work)
- Einheitliches Treibermodell über das `Device`-Abstraktionslayer
- Device-Tree (DTS) zur Hardware-Beschreibung, getrennt von der Firmware-Logik
- Kconfig als Build-Konfigurationssystem (aus dem Linux-Kernel übernommen)
- West als Meta-Build-Tool und Multi-Repository-Manager

**nRF Connect SDK (NCS)** ist Nordics Erweiterungsschicht auf Zephyr:

- Enthält nRF-spezifische Treiber, Libraries und Protokoll-Stacks
- Kapselt den Nordic Softdevice als BLE-Controller-Option sowie Zephyrs eigenen BLE-Host
- Bietet Matter, Thread, Zigbee, LTE-M/NB-IoT/GPS-Libraries
- Enthält MCUboot als Bootloader und nRF Connect Device Manager
- NCS-Versions-Tags entsprechen Zephyr-Revisions-Pins (z. B. NCS v2.6.0 → Zephyr 3.5.99-ncs1)

---

## 2. Build-System & West

### West-Workspace-Layout

```
<workspace>/
├── .west/
│   └── config               ← lokale West-Konfiguration
├── nrf/                     ← sdk-nrf (NCS)
├── zephyr/                  ← sdk-zephyr
├── modules/                 ← externe Module (CMSIS, hal_nordic, …)
│   └── hal/nordic/
├── bootloader/mcuboot/
└── my-app/                  ← eigene Applikation (self.path in west.yml)
```

### west.yml (Applikations-Manifest)

```yaml
manifest:
  remotes:
    - name: nrfconnect
      url-base: https://github.com/nrfconnect
  projects:
    - name: sdk-nrf
      remote: nrfconnect
      revision: v2.6.0
      import: true        # importiert Zephyr + alle NCS-Abhängigkeiten
  self:
    path: my-app
```

### Build-Befehle

```bash
# Einmalig: Workspace initialisieren
west init -l my-app/
west update

# Applikation bauen
west build -b nrf52840dk/nrf52840  samples/blinky

# Flash
west flash

# Debug (JLink)
west debug

# Build-Verzeichnis leeren
west build -t pristine
```

---

## 3. Projektstruktur einer Zephyr-Applikation

```
my-app/
├── CMakeLists.txt           ← Pflicht: find_package(Zephyr) + app-Target
├── prj.conf                 ← Kconfig-Defaults für alle Boards
├── Kconfig                  ← App-eigene Kconfig-Symbole (optional)
├── src/
│   └── main.c
└── boards/
    ├── nrf52840dk_nrf52840.conf      ← board-spezifisches Kconfig
    └── nrf52840dk_nrf52840.overlay   ← board-spezifisches DTS-Overlay
```

### CMakeLists.txt (Minimal)

```cmake
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)
target_sources(app PRIVATE src/main.c)
```

---

## 4. Device Tree (DTS)

Device Tree beschreibt die Hardware **deklarativ** getrennt vom C-Code.

### Kernkonzepte

| Konzept | Beschreibung |
|---------|-------------|
| **Node** | Hardware-Gerät (`&i2c0`, `&gpio0`, …) |
| **Property** | Eigenschaft eines Node (`reg`, `status`, `compatible`) |
| **Binding** | YAML-Schema, das festlegt welche Properties ein `compatible`-Wert erfordert |
| **Overlay** | Ergänzungs-DTS, das Board-DTS erweitert oder überschreibt |
| **Alias** | Symbolischer Name (`led0`, `sw0`) |
| **Chosen** | Globale Zuordnungen (`zephyr,console`, `zephyr,bt-uart`) |

### Typisches Overlay für I2C-Sensor

```dts
/* boards/nrf52840dk_nrf52840.overlay */
&i2c0 {
    status = "okay";
    my_sensor: bme280@76 {
        compatible = "bosch,bme280";
        reg = <0x76>;
        status = "okay";
    };
};

/ {
    aliases {
        my-sensor = &my_sensor;
    };
};
```

### DTS-Makros im C-Code

```c
/* Gerät per DT-Alias holen */
const struct device *dev = DEVICE_DT_GET(DT_ALIAS(my_sensor));

/* Gerät per compatible holen (erstes passendes) */
const struct device *dev = DEVICE_DT_GET_ONE(bosch_bme280);

/* GPIO aus DT-Alias */
static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Bereitschaft prüfen */
if (!device_is_ready(dev)) { /* Fehlerbehandlung */ }
```

---

## 5. Kconfig

Kconfig steuert, welche Zephyr-Subsysteme und Treiber kompiliert werden.

### Wichtige Symbole

```kconfig
# Bluetooth
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y          # BLE Peripheral-Rolle
CONFIG_BT_OBSERVER=y            # Scanner
CONFIG_BT_DEVICE_NAME="MyDevice"

# I2C
CONFIG_I2C=y

# Sensor-Framework
CONFIG_SENSOR=y

# Logging
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3      # 0=off 1=err 2=warn 3=inf 4=dbg

# Energie-Sparen
CONFIG_PM=y
CONFIG_PM_DEVICE=y

# Stack-Größen (Anpassung bei Overflow-Fehlern)
CONFIG_MAIN_STACK_SIZE=2048
CONFIG_BT_RX_STACK_SIZE=1024
```

### Board-spezifisches Kconfig (NCS)

```kconfig
# Für Seeed XIAO nRF52840: interner RC-Oszillator statt ext. XTAL
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_ACCURACY_500=y
```

---

## 6. BLE-Stack (Zephyr / NCS)

### Architektur

```
Application
    │
Zephyr BT Host  (HCI-Host-Layer, GATT, GAP, L2CAP, SMP)
    │
Nordic BLE Controller  (SoftDevice Controller oder Zephyr LL)
    │
nRF52840 Radio Hardware
```

### BLE Advertising (Non-Connectable)

```c
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_USE_IDENTITY,     /* Options */
        BT_GAP_ADV_SLOW_INT_MIN,        /* min interval (160 × 0.625 ms = 100 ms) */
        BT_GAP_ADV_SLOW_INT_MAX,        /* max interval */
        NULL);                          /* Kein Peer (Broadcast) */

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_SVC_DATA16, my_svc_data, sizeof(my_svc_data)),
};

/* BLE initialisieren */
bt_enable(NULL);

/* Advertising starten */
bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);

/* Advertising-Daten aktualisieren (ohne Stopp) */
bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
```

### AD-Typen (BT_DATA_*)

| Makro | Wert | Bedeutung |
|-------|------|-----------|
| `BT_DATA_FLAGS` | 0x01 | LE-Flags |
| `BT_DATA_UUID16_ALL` | 0x03 | Vollständige 16-Bit-UUIDs |
| `BT_DATA_NAME_COMPLETE` | 0x09 | Vollständiger Gerätename |
| `BT_DATA_SVC_DATA16` | 0x16 | Service Data (16-Bit UUID) |

---

## 7. Sensor-Framework

```c
#include <zephyr/drivers/sensor.h>

const struct device *imu = DEVICE_DT_GET_ONE(invensense_mpu6050);

/* Daten abholen (Treiberabruf) */
sensor_sample_fetch(imu);

/* Kanal lesen */
struct sensor_value accel[3];
sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel);
/* accel[i].val1 = ganzzahliger Teil, accel[i].val2 = Bruchteile (µ-Einheiten) */

/* Umrechnung in Integer (z. B. mm/s²) */
int32_t ax_mms2 = (int32_t)(accel[0].val1 * 1000 +
                             accel[0].val2 / 1000);
```

### Verfügbare Kanäle (`SENSOR_CHAN_*`)

| Kanal | Beschreibung |
|-------|-------------|
| `SENSOR_CHAN_ACCEL_XYZ` | Beschleunigung X/Y/Z (m/s²) |
| `SENSOR_CHAN_GYRO_XYZ` | Winkelgeschwindigkeit X/Y/Z (rad/s) |
| `SENSOR_CHAN_DIE_TEMP` | Chip-Temperatur (°C) |
| `SENSOR_CHAN_AMBIENT_TEMP` | Umgebungstemperatur |
| `SENSOR_CHAN_HUMIDITY` | Rel. Feuchte (%) |
| `SENSOR_CHAN_PRESS` | Luftdruck (kPa) |

---

## 8. GPIO

```c
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec pir =
    GPIO_DT_SPEC_GET(DT_ALIAS(pir_sensor), gpios);

/* Initialisieren */
gpio_pin_configure_dt(&pir, GPIO_INPUT);

/* Lesen */
int state = gpio_pin_get_dt(&pir);

/* Interrupt konfigurieren */
static struct gpio_callback pir_cb_data;

void pir_handler(const struct device *dev, struct gpio_callback *cb,
                 uint32_t pins)
{
    printk("PIR triggered!\n");
}

gpio_pin_interrupt_configure_dt(&pir, GPIO_INT_EDGE_BOTH);
gpio_init_callback(&pir_cb_data, pir_handler, BIT(pir.pin));
gpio_add_callback(pir.port, &pir_cb_data);
```

---

## 9. Zephyr-Kernel-Primitives

```c
#include <zephyr/kernel.h>

/* Sleep */
k_sleep(K_MSEC(100));
k_sleep(K_SECONDS(10));

/* Work Queue (deferred/delayed execution) */
static struct k_work_delayable my_work;

void my_worker(struct k_work *work) { /* … */ }

k_work_init_delayable(&my_work, my_worker);
k_work_schedule(&my_work, K_SECONDS(5));

/* Semaphore */
K_SEM_DEFINE(my_sem, 0, 1);
k_sem_give(&my_sem);
k_sem_take(&my_sem, K_FOREVER);
```

---

## 10. Zephyr-Modul einbinden

Ein Zephyr-Modul ist eine externe Library, die sich in den West-Workspace einklinkt.

### `zephyr/module.yml`

```yaml
name: my_module
build:
  cmake: .       # Pfad zu CMakeLists.txt (relativ zu module.yml)
  kconfig: Kconfig
```

### `CMakeLists.txt` (Modul-Root)

```cmake
if(CONFIG_MY_MODULE)
  add_subdirectory(lib/my_lib)
endif()
```

### Library-`CMakeLists.txt`

```cmake
zephyr_library()
zephyr_library_sources(src/my_lib.c)
zephyr_library_include_directories(include)
```

### Library-`Kconfig`

```kconfig
config MY_LIB
    bool "Enable My Library"
    depends on BT
    help
      Enables the My Library for BLE sensor data encoding.
```

---

## 11. Logging & Debugging

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_module, LOG_LEVEL_DBG);

LOG_INF("Temperature: %d.%02d °C", val.val1, val.val2 / 10000);
LOG_WRN("Queue full, dropping measurement");
LOG_ERR("Device not ready: %d", ret);
```

Ausgabe über RTT (J-Link) oder UART:
```bash
west build -b nrf52840dk/nrf52840 -- -DCONFIG_USE_SEGGER_RTT=y
JLinkRTTClient
```

---

## 12. Board-Identifikatoren (NCS v2.6+)

| Hardware | Board-String |
|---------|-------------|
| nRF52840-DK | `nrf52840dk/nrf52840` |
| Seeed XIAO nRF52840 | `seeed_xiao_ble/nrf52840` |
| Seeed XIAO nRF52840 Sense | `seeed_xiao_ble_sense/nrf52840` |
| nRF52833-DK | `nrf52833dk/nrf52833` |
| nRF5340-DK (App-Core) | `nrf5340dk/nrf5340/cpuapp` |

> **Hinweis:** Ab NCS v2.5 verwenden Board-Strings das Format `board/soc` statt des
> alten `board_soc` (Unterstrich). Beide Formate werden noch akzeptiert.

---

## 13. Häufige Fehler & Lösungen

| Fehler | Ursache | Lösung |
|--------|---------|--------|
| `device not ready` | Treiber nicht enabled oder Overlay fehlt | `status = "okay"` im DTS-Overlay prüfen |
| BLE-Stack hängt | LFCLK-Quelle fehlt (kein ext. XTAL) | `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` setzen |
| `CONFIG_BT` missing | Bluetooth nicht konfiguriert | `CONFIG_BT=y` in `prj.conf` |
| Stack-Overflow | Stack zu klein | `CONFIG_MAIN_STACK_SIZE` erhöhen, `CONFIG_DEBUG_OPTIMIZATIONS=y` |
| `west update` schlägt fehl | Falscher `revision`-Tag in `west.yml` | Gültigen NCS-Tag prüfen: `git tag -l` in `nrf/` |
| Linker-Error: symbol nicht gefunden | Library nicht eingebunden | `CMakeLists.txt` und `Kconfig`-Abhängigkeiten prüfen |

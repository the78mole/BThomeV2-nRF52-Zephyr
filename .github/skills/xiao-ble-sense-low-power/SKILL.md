---
name: xiao-ble-sense-low-power
description: >-
  Domain knowledge for power optimization of the Seeed XIAO BLE Sense (nRF52840)
  under Zephyr NCS. Covers System OFF, QSPI Flash Deep Power-Down (P25Q16H),
  GPIO bitbang DPD, hardware floor, Device Tree overlays, and PPK2 measurement.
  Use this skill for: low-power optimization; System OFF implementation;
  QSPI flash power-down; GPIO hog configuration; current measurement with nRF PPK2;
  XIAO BLE Sense pin mapping; µA measurements; battery lifetime estimation.
user-invocable: true
---

# XIAO BLE Sense — Low-Power Optimization (Zephyr NCS)

---

## 1. Hardware Basics

### Board: Seeed Studio XIAO BLE Sense

| Property | Value |
|---|---|
| SoC | Nordic nRF52840 |
| Flash (external) | Puya P25Q16H (128 Mbit QSPI) |
| Bootloader | Adafruit nRF52840 UF2 (0xADA52840) |
| App address | `0x27000` |
| Zephyr board name | `xiao_ble_sense` |

### QSPI Flash Pin Mapping (P25Q16H)

**CRITICAL:** This mapping deviates from intuitive expectations.

| Signal | Pin | Direction | Note |
|---|---|---|---|
| QSPI_CSN | **P0.25** | OUT | **Active-LOW** (chip select) |
| QSPI_SCK | P0.21 | OUT | Clock |
| QSPI_IO0 / MOSI | P0.20 | OUT | SPI data (bitbang: MOSI) |
| QSPI_IO1 / MISO | P0.24 | IN | SPI data (bitbang: MISO, not needed for DPD) |
| QSPI_IO2 | P0.22 | — | Quad mode (not needed for DPD) |
| QSPI_IO3 | P0.23 | — | Quad mode (not needed for DPD) |

Source: `xiao_ble-pinctrl.dtsi` — `NRF_PSEL(QSPI_CSN, 0, 25)`

### Hardware Floor (unavoidable quiescent current without HW modification)

The XIAO BLE Sense has external consumers that cannot be disabled in software:

- **Charge IC** (Seeed onboard): ~20–30 µA
- **RGB LED pull-ups** / passive circuitry
- **LDO / DC-DC regulator**

**Measured hardware floor: ~44.7 µA** (System OFF + flash DPD + IMU/MIC disabled via GPIO)

> The nRF52840 itself draws < 2 µA in System OFF. The remaining 42+ µA
> come from the XIAO hardware — only reducible by hardware modification.

---

## 2. Known Current Traps — Root Causes

### 🔴 Trap 1: GPIO Hog P0.25 OUTPUT-LOW → ~600 µA continuous current

**Problem:** P0.25 = QSPI_CSN (active-LOW). A GPIO hog with `output-low` on P0.25
holds the chip select permanently active → P25Q16H stays selected → ~600 µA drain.

**Wrong (never use):**
```dts
/* ❌ WRONG — keeps flash permanently selected */
&gpio0 {
    qspi-csn-low {
        gpio-hog;
        gpios = <25 GPIO_ACTIVE_HIGH>;
        output-low;
    };
};
```

**Correct:** Never hog P0.25. Power down flash via DPD command (→ Section 4).

---

### 🔴 Trap 2: `CONFIG_NORDIC_QSPI_NOR=y` → 9.5 mA continuous current

**Problem:** The Zephyr driver `nordic_qspi-nor` performs JEDEC ID read and SFDP read
sequences during initialization. This requires the **HFXO** (High Frequency Crystal
Oscillator). The HFXO stays active → `sys_poweroff()` is blocked or never reached
→ SoC runs at full clock speed → 9.5 mA continuous current.

**Wrong:**
```kconfig
# ❌ Causes 9.5 mA, sys_poweroff() is never reached
CONFIG_NORDIC_QSPI_NOR=y
```

**Correct:** Disable the QSPI driver entirely, send DPD via GPIO bitbang (→ Section 4).

---

### 🔴 Trap 3: `CONFIG_PM_DEVICE_RUNTIME=y` → 10 mA / crash loop

**Problem:** `PM_DEVICE_RUNTIME` creates work queue threads. With a small stack
(`CONFIG_MAIN_STACK_SIZE=1024`) → stack overflow → crash loop → high continuous current.

**Correct:** Omit `PM_DEVICE_RUNTIME`. No PM framework needed for a one-shot power-down.

---

### 🟡 Trap 4: IMU and microphone power supply

| Consumer | Pin | Action |
|---|---|---|
| LSM6DS3TR-C IMU VCC | P1.08 | GPIO hog OUTPUT-LOW |
| MSM261D3526H1 Microphone VCC | P1.10 | GPIO hog OUTPUT-LOW |

```dts
/* ✅ Correct: disable IMU and MIC via hog */
&gpio1 {
    pwr-mic {
        gpio-hog;
        gpios = <10 GPIO_ACTIVE_HIGH>;
        output-low;
        line-name = "MIC Power";
    };
    pwr-imu {
        gpio-hog;
        gpios = <8 GPIO_ACTIVE_HIGH>;
        output-low;
        line-name = "IMU Power";
    };
};
```

---

## 3. Reference Configuration (Sample 099)

### `prj.conf` — minimal base

```kconfig
CONFIG_GPIO=y
CONFIG_TICKLESS_KERNEL=y
CONFIG_MAIN_STACK_SIZE=1024
CONFIG_IDLE_STACK_SIZE=512
```

### `boards/xiao_ble_sense.conf`

```kconfig
# RC oscillator instead of XTAL for the 32 kHz clock (saves ~200 µA in sleep)
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y

# Disable console, USB, logging, and sensor drivers
CONFIG_USB_DEVICE_STACK=n
CONFIG_USB_UART_CONSOLE=n
CONFIG_SERIAL=n
CONFIG_CONSOLE=n
CONFIG_UART_CONSOLE=n
CONFIG_LOG=n
CONFIG_SENSOR=n
CONFIG_LSM6DSL=n

# Release NFC pins as GPIOs (otherwise P0.09/P0.10 are reserved as NFC antenna)
CONFIG_NFCT_PINS_AS_GPIOS=y
```

### `boards/xiao_ble_sense.overlay` — relevant QSPI section

```dts
/* ── Disable QSPI peripheral ────────────────────────────────────────────
 * The Zephyr nordic_qspi-nor driver is NOT used.
 * Deep Power-Down (0xB9) is sent via GPIO bitbang in main.c.
 * WARNING: P0.25 = QSPI_CSN (active-LOW) — never hog OUTPUT-LOW!
 */
&qspi { status = "disabled"; };
```

---

## 4. P25Q16H Flash — Deep Power-Down via GPIO Bitbang

This is the only reliable way to put the flash into DPD before `sys_poweroff()`
without activating the Zephyr QSPI driver (which would otherwise hold the HFXO).

### Background

- **DPD opcode:** `0xB9`
- **t_enter_dpd:** 3 µs (from DT: `t-enter-dpd = <3000>` ns)
- **t_exit_dpd:** 8 ms (from DT: `t-exit-dpd = <8000>` ns)
- **SPI mode:** Mode 0 (CPOL=0, CPHA=0), MSB-first
- **Protocol:** CS low → 8 bits → CS high → wait t_enter_dpd

### C Implementation (direct nRF HAL usage)

```c
#include <hal/nrf_gpio.h>

/* P25Q16H QSPI pins on XIAO BLE Sense */
#define FLASH_PIN_MOSI  20u   /* P0.20 = QSPI_IO0 */
#define FLASH_PIN_SCK   21u   /* P0.21 = QSPI_SCK */
#define FLASH_PIN_CSN   25u   /* P0.25 = QSPI_CSN (active-LOW!) */
#define FLASH_CMD_DPD   0xB9u

static void flash_deep_power_down(void)
{
    /* Configure pins as outputs */
    nrf_gpio_cfg_output(FLASH_PIN_CSN);
    nrf_gpio_cfg_output(FLASH_PIN_SCK);
    nrf_gpio_cfg_output(FLASH_PIN_MOSI);

    /* Idle state: CSN HIGH (deselected), SCK LOW */
    nrf_gpio_pin_set(FLASH_PIN_CSN);
    nrf_gpio_pin_clear(FLASH_PIN_SCK);
    nrf_gpio_pin_clear(FLASH_PIN_MOSI);

    /* Assert CS (select flash) */
    nrf_gpio_pin_clear(FLASH_PIN_CSN);

    /* Send 0xB9 MSB-first via SPI Mode 0 */
    for (int i = 7; i >= 0; i--) {
        nrf_gpio_pin_write(FLASH_PIN_MOSI, (FLASH_CMD_DPD >> i) & 1u);
        nrf_gpio_pin_set(FLASH_PIN_SCK);    /* rising edge — data is latched */
        nrf_gpio_pin_clear(FLASH_PIN_SCK);  /* falling edge */
    }

    /* Deassert CS → DPD command complete */
    nrf_gpio_pin_set(FLASH_PIN_CSN);

    /* Wait t_enter_dpd = 3 µs (conservative: 10 µs) */
    k_busy_wait(10u);
}
```

**Call in main.c** (before `sys_poweroff()`):
```c
flash_deep_power_down();
sys_poweroff();
```

**No include overhead:** `<hal/nrf_gpio.h>` is always available, requires no Kconfig symbol.

---

## 5. System OFF Checklist

Before calling `sys_poweroff()`, all consumers must be powered down:

- [ ] **QSPI flash:** DPD via bitbang (Section 4)
- [ ] **IMU LSM6DS3TR-C:** GPIO hog P1.08 OUTPUT-LOW
- [ ] **Microphone MSM261D3526H1:** GPIO hog P1.10 OUTPUT-LOW
- [ ] **QSPI driver:** `&qspi { status = "disabled"; }` in overlay
- [ ] **`CONFIG_NORDIC_QSPI_NOR`:** must **not** be set
- [ ] **`CONFIG_PM_DEVICE_RUNTIME`:** must **not** be set
- [ ] **`CONFIG_NFCT_PINS_AS_GPIOS=y`:** set (otherwise P0.09/P0.10 are used as NFC)
- [ ] **32 kHz clock:** `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`

**Expected result:** ~44.7 µA (hardware floor of the XIAO BLE Sense)

---

## 6. PPK2 Measurement and Analysis

### Measurement

- **Mode:** Source Mode, 3.3 V
- **Connection:** V+ → XIAO 3V3 pin, GND → XIAO GND
- **Software:** nRF Power Profiler (Desktop) or nRF PPK2 app
- **D0 digital input:** GPIO of the nRF52840 (e.g. P0.05 = D5) for timestamps

### Analysis with the analysis script

```bash
uv run scripts/ppk_analysis.py data/<file>.ppk2
```

The script provides:
- Total duration, sample rate, mean, peak
- **Boot phase** (0–1010 ms) is automatically excluded
- **Steady-state mean** (after boot phase) = the relevant figure
- D0-HIGH phase analysis (timestamps, energy)
- Battery lifetime estimate for CR2032, 2× AA, 2× AAA

### Reference measurement (Sample 099, GPIO bitbang DPD)

| Metric | Value |
|---|---|
| Steady-state avg | **44.70 µA** |
| Boot peak | 79.5 mA (short burst) |
| D0-HIGH duration | 50 ms (boot marker) |
| D0-HIGH avg current | 2338 µA |
| CR2032 (230 mAh) | ~214 days |
| 2× AA (2500 mAh) | ~6.4 years |

---

## 7. Flash Workflow (XIAO BLE Sense)

```bash
# Build + flash (default port /dev/ttyACM3)
make 099-sysoff-serial-flash BOARD=xiao_ble_sense

# Specify a different port
make 099-sysoff-serial-flash BOARD=xiao_ble_sense SERIAL_PORT=/dev/ttyACM1
```

> **Prerequisite:** Double-click RESET until the LED fades (DFU mode), then flash.

### Flash Toolchain

- **Tool:** `adafruit-nrfutil 0.5.3.post16`
- **Installation:** `uv tool install adafruit-nrfutil`
- **UF2 family:** `0xADA52840`
- **App address:** `0x27000`

---

## 8. Lessons Learned (Session Summary)

| # | Finding |
|---|---|
| 1 | P0.25 on XIAO BLE Sense is **QSPI_CSN** (active-LOW), not a VCC enable |
| 2 | GPIO hog OUTPUT-LOW on P0.25 → flash always selected → ~600 µA |
| 3 | `CONFIG_NORDIC_QSPI_NOR=y` holds HFXO after init → sys_poweroff() unreachable → 9.5 mA |
| 4 | `CONFIG_PM_DEVICE_RUNTIME=y` → work queue + stack overflow → crash loop → ~10 mA |
| 5 | GPIO bitbang (direct nRF HAL) is more reliable than Zephyr PM framework for a one-shot power-down |
| 6 | This pattern matches exactly the Adafruit Arduino sketch (`transport.runCommand(0xB9)` before SYSTEMOFF) |
| 7 | Hardware floor of the XIAO BLE Sense: ~44.7 µA — only reducible by hardware modification |

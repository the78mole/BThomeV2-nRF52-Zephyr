# 099 — XIAO Power Baseline Tests

Minimal baseline application for characterising the **idle floor** of the
Seeed XIAO nRF52840 / nRF52840 Sense board.

The firmware does **nothing** after boot:

1. Drives `D5` (P0.05) HIGH for 50 ms — boot-done marker on the PPK2.
2. Drives `D5` LOW.
3. Calls `k_sleep(K_FOREVER)` — kernel idle thread enters WFI.

No Bluetooth, no sensors, no serial, no logging.  This isolates the
contribution of the SoC + onboard regulators + onboard peripherals from
any firmware activity.

## Build & flash

```sh
make 099-build BOARD=xiao_ble_sense
make 099-flash BOARD=xiao_ble_sense
```

## Measurement procedure

> **CRITICAL — first flash only:** This sample sets `CONFIG_NFCT_PINS_AS_GPIOS=y`
> which patches `UICR.NFCPINS`.  UICR is **non-volatile** and only re-read at
> a full power-on reset.  After the **first** flash you MUST:
>
> 1. unplug USB
> 2. disconnect any battery / external supply
> 3. wait ~5 s
> 4. reconnect supply
>
> Otherwise the NFCT block stays active and the floor remains ~600–700 µA.
> Subsequent flashes do not need this dance — the UICR is already patched.

1. Connect the PPK2 in **Source-meter mode**, 3.3 V to the XIAO `VBAT` pad
   (USB disconnected — USB SOF IRQs alone add ~1.1 mA).
2. Optionally wire `D5` (P0.05) to a PPK2 logic input.
3. Record for ≥30 s, then run:
   ```sh
   uv run scripts/ppk_analysis.py data/<recording>.ppk2
   ```
4. The steady-state average **after** the D5 falling edge is the idle floor.

## Variables to characterise

Toggle one option at a time, rebuild, flash, re-measure.

### Overlay (`boards/xiao_ble_sense.overlay`)

| Node / hog                       | Default in this sample | Try also              |
|----------------------------------|------------------------|-----------------------|
| `&i2c0` / `&i2c1`                | disabled               | enable individually   |
| `&qspi`                          | disabled               | enable (no hog)       |
| `&pwm0` / `&spi2`                | disabled               | enable                |
| `pwr-flash` (P0.25 hog, low)     | LOW = flash off        | remove / set HIGH     |
| `pwr-mic`   (P1.10 hog, low)     | LOW = mic off          | remove / set HIGH     |
| `lsm6ds3tr-c-en` regulator       | `regulator-boot-on` removed → IMU off | keep boot-on |

### Kconfig (`boards/xiao_ble_sense.conf`)

| Option                       | Default in this sample | Effect when ON          |
|------------------------------|------------------------|-------------------------|
| `CONFIG_USB_DEVICE_STACK=n`  | OFF                    | +~1.1 mA (USB SOF IRQ)  |
| `CONFIG_SERIAL=n`            | OFF                    | +~1.0 mA (UARTE0 HFINT) |
| `CONFIG_CONSOLE=n`           | OFF                    | pulls in serial         |
| `CONFIG_LOG=n`               | OFF                    | log work queue wakes    |

### Hardware-level notes (cannot be changed from firmware)

- **Charger BQ25101**: quiescent ~10 µA, draws charge current when USB is
  connected.  Use battery / bench supply on VBAT, NOT USB, for low-floor
  measurements.
- **Power LED (red)** on `VBAT` rail of XIAO `Sense`: ~700 µA.  Must be
  desoldered for sub-mA measurements.
- **3V3 LDO XC6220** quiescent: ~10 µA.

## Expected order of magnitude

With everything off (this baseline) on a XIAO `Sense` with power-LED
**still soldered**:

- ~700 µA (LED dominates)

With power-LED removed:

- ~5–15 µA (SoC WFI + LDO + charger quiescent)

# Low-Power Management on nRF52840 with Zephyr / NCS

> 🌐 [Deutsche Version](../de/low_power_mgmt.md)

This document captures all findings from a systematic power-optimisation session
on the **Seeed XIAO nRF52840** (plain and Sense) and the **nRF52840-DK** running
NCS 2.6.0 / Zephyr 3.5.99-ncs1.

The goal was to reach **System ON / CPU asleep** idle current (< 20 µA) between
30-second BThome V2 advertising bursts.  The investigation was driven by
real-time current measurements with a **Nordic PPK2** and a GPIO active-indicator
(`active_ind`, P0.02/D0 on XIAO, P1.02/D1 on DK) that marks awake vs. sleep
phases in the PPK2 logic trace.

---

## Measurement Setup

### PPK2 Active-Indicator

A dedicated GPIO is driven HIGH while the CPU is executing and LOW just before
`k_sleep()`.  This creates a correlated logic trace in the PPK2 that maps
current spikes to exact code phases.

| Board | GPIO | Connector label |
|---|---|---|
| Seeed XIAO nRF52840 | P0.02 | D0 |
| nRF52840-DK | P1.02 | Arduino D1 |

DTS alias: `active-indicator` → `active0` node (see board overlays).

### Final Measured Results (nRF52840-DK, no external parasites)

| Phase | Duration | Current |
|---|---|---|
| Boot + `bt_enable()` | ~2.5 s | 3–4 mA |
| BLE advertising burst (D0=H) | 3 s | ~1.0 mA |
| `k_sleep(27 s)` (D0=L) | 27 s | **~28 µA** base, ~60 µA average |
| Cycle average | 30 s | ~130 µA |

> **Note:** The XIAO nRF52840 has additional parasitic loads from the PCB
> (USB regulator, charge IC, RGB LED pull-ups) that add ~50–100 µA on top.
> The DK numbers are cleaner but also not ideal due to its own parasites.

---

## Root-Cause Analysis — All Identified HFCLK Holders

The nRF52840 draws ~3–8 µA in *System ON / CPU asleep* mode.  Any hardware
peripheral that holds a 16 MHz HFINT (or HFXO) clock request prevents the CPU
from reaching that state and instead causes ~600–1100 µA idle current.

### 1. USB Device Stack (~1.1 mA)

**Symptom:** ~1.1 mA constant after boot, even without USB cable.  
**Cause:** The XIAO board defconfig enables `CONFIG_USB_DEVICE_STACK=y` by
default.  The USB SoF interrupt fires every 1 ms and keeps the HFXO (64 MHz)
running permanently.  
**Fix:**
```kconfig
CONFIG_USB_DEVICE_STACK=n
```

### 2. UARTE0 / Serial (~1.0 mA)

**Symptom:** ~1 mA constant, persists even after USB is disabled.  
**Cause:** The nrfx UARTE driver calls `nrfx_uarte_init()` at boot, which
requests the 16 MHz HFINT oscillator via the clock-control subsystem.  The
request is never released as long as the driver is active, even during idle.  
**Fix:**
```kconfig
CONFIG_SERIAL=n
CONFIG_CONSOLE=n
CONFIG_UART_CONSOLE=n
CONFIG_LOG=n
```

### 3. bt_le_adv_stop() does NOT release HFCLK (~640 µA)

**Symptom:** After `bt_le_adv_stop()` the current stays at ~640 µA — identical
to the advertising phase.  
**Cause:** `bt_le_adv_stop()` stops the advertising state machine in the BLE
host but does not tear down the SoftDevice Controller (SDC).  The SDC keeps
its internal HFCLK request active as long as `bt_enable()` is in effect.  
**Fix:** Call `bt_disable()` after `bt_le_adv_stop()`.  `bt_disable()` shuts
down the SDC (`sdc_disable()`) which would in principle release the clock —
but see item 4.

### 4. MPSL holds HFCLK after bt_disable() (~640 µA)

**Symptom:** `bt_disable()` returns but current stays at ~640 µA.  
**Cause:** The MultiProtocol Service Layer (MPSL) manages radio hardware and
HFCLK on behalf of the SDC.  When `bt_disable()` is called, `sdc_disable()` is
executed, but MPSL itself is only torn down if
`CONFIG_BT_UNINIT_MPSL_ON_DISABLE=y` is set **and** its full dependency chain
is satisfied.

`CONFIG_BT_UNINIT_MPSL_ON_DISABLE` depends on `CONFIG_MPSL_DYNAMIC_INTERRUPTS`,
which depends on `CONFIG_DYNAMIC_DIRECT_INTERRUPTS`, which depends on
`CONFIG_DYNAMIC_INTERRUPTS`.  If any of the four is missing, **Kconfig silently
ignores the setting** — there is no warning.

**Fix — full dependency chain:**
```kconfig
CONFIG_DYNAMIC_INTERRUPTS=y
CONFIG_DYNAMIC_DIRECT_INTERRUPTS=y
CONFIG_MPSL_DYNAMIC_INTERRUPTS=y
CONFIG_BT_UNINIT_MPSL_ON_DISABLE=y
```

Verification — after build, check `.config`:
```bash
grep -E "BT_UNINIT_MPSL|MPSL_DYNAMIC|DYNAMIC_DIRECT|DYNAMIC_INTERRUPTS" build/zephyr/.config
# Expected: all four = y
```

### 5. QSPI Flash Peripheral (~200 µA) — XIAO only

**Symptom:** ~200 µA residual base current on the XIAO even with BLE off.  
**Cause:** The P25Q16H QSPI NOR flash is defined with `status = "okay"` in
`xiao_ble_common.dtsi`.  The Nordic QSPI driver initialises the QSPI peripheral
and sets up a DMA-ready state at boot, even if no physical flash chip is
present.  This holds the HFINT oscillator active.  
**Fix (overlay):**
```dts
&qspi { status = "disabled"; };
```

### 6. TWI/I2C1 Peripheral (~630 µA) — XIAO and DK

**Symptom:** ~630 µA base current immediately after boot, before BLE is even
started.  This was the dominant and most confusing finding: the current was
identical before and after `bt_disable()`, suggesting BLE was irrelevant.  
**Cause:** `i2c1` (`nordic,nrf-twi`, P0.04/P0.05 on XIAO) is enabled in
`xiao_ble_common.dtsi` with `status = "okay"`.  The nrfx TWI driver calls
`nrfx_twi_enable()` during its `init()` callback, which sets the TWI ENABLE
register and registers a permanent HFCLK request.  Since the application never
uses `i2c1` (the IMU uses `i2c0`) and never calls `pm_device_action_run()` on
it, the request is never released.  
**Fix (overlay):**
```dts
&i2c1 { status = "disabled"; };
```

> **Key insight:** Any nrfx peripheral driver (`nrfx_twi_enable`,
> `nrfx_spi_enable`, `nrfx_qspi_init`, …) that is initialised by Zephyr at
> boot holds a HFCLK request for its entire lifetime, regardless of whether the
> application ever transfers data.  Disable every DT node that is not actively
> used.

### 7. LFRC Calibration Timer blocks HFCLK (~630 µA + 500 µA spike)

**Symptom (observed on XIAO):** After a clean sleep phase of ~5 s at 630 µA,
the current suddenly jumps to ~1.1 mA and stays there permanently.  The D0
active-indicator is LOW throughout — the CPU is sleeping.  
**Cause:** Zephyr's `nrf_clock_calibration.c` fires a `k_timer` every
`CONFIG_CLOCK_CONTROL_NRF_CALIBRATION_PERIOD` ms (default: 4000 ms).  When
`CONFIG_CLOCK_CONTROL_NRF_CALIBRATION_MAX_SKIP > 0`, the code compiles with
`USE_TEMP_SENSOR=1`, meaning it calls `mpsl_temperature_get()` before each
calibration.

After `mpsl_lib_uninit()` (via `BT_UNINIT_MPSL_ON_DISABLE`), MPSL's internal
state is torn down.  The calibration callback calls `mpsl_temperature_get()`
into an uninitialised MPSL → the call blocks indefinitely inside
`start_cycle()` → `hf_release()` is never reached → HFCLK is held permanently
at ~630 µA.  A second timer fire at +4000 ms stacks another HFCLK request →
+500 µA jump to ~1.1 mA.

The jump at exactly **+4942 ms after bt_disable()** (measured via PPK2 CSV
analysis) confirmed this: `BT_CONN_PARAM_UPDATE_TIMEOUT = 5000 ms` was
initially suspected, but the precise timer period and the USE_TEMP_SENSOR path
proved to be the real culprit.

MPSL manages LFRC calibration internally during active BLE phases anyway; the
Zephyr-level calibration is redundant in this configuration.  
**Fix:**
```kconfig
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION=n
```

### 8. Microphone Power Rail P1.10 (~100 µA) — XIAO Sense only

**Symptom:** ~100 µA additional base current on the Sense variant.  
**Cause:** The MSM261D3526H1CPM PDM microphone is powered via a
`regulator-fixed` node in the board DTS with `regulator-boot-on`, which drives
P1.10 HIGH immediately after boot.  
**Fix (overlay):**
```dts
/ {
    mic-vdd-off {
        compatible = "regulator-fixed";
        regulator-name = "MIC_PWR_OFF";
        enable-gpios = <&gpio1 10 GPIO_ACTIVE_HIGH>;
        regulator-always-off;
    };
};
```

---

## PPK2 Trace Analysis (CSV)

The PPK2 exports CSV files with columns `Timestamp(ms)`, `Current(uA)`,
`D0-D7`.  The following Python snippet was used to correlate current phases with
code execution:

```python
import csv

timestamps, currents, d0 = [], [], []
with open('data/ppk-YYYYMMDDTHHMMSS.csv') as f:
    for row in csv.DictReader(f):
        timestamps.append(int(row['Timestamp(ms)']))
        currents.append(float(row['Current(uA)']))
        d0.append(row['D0-D7'][0])  # D0 = active_indicator

# Per-second average with D0 state
for sec in range(int(timestamps[-1]/1000) + 1):
    vals = [currents[i] for i in range(len(timestamps))
            if sec*1000 <= timestamps[i] < (sec+1)*1000]
    d0v  = [d0[i]       for i in range(len(timestamps))
            if sec*1000 <= timestamps[i] < (sec+1)*1000]
    if vals:
        print(f"t={sec:3}s  avg={sum(vals)/len(vals):7.1f} uA  "
              f"D0={'H' if d0v.count('1')/len(d0v)>0.5 else 'L'}")
```

### Annotated trace from `ppk-20260504T121124.csv`

```
t= 0s    0.0 µA   D0=L   PPK2 recording starts, device not yet powered
t= 1s    ---      D0=L   Boot: Zephyr kernel init
t= 2s   3590 µA   D0=L   bt_enable() running (MPSL + SDC init)
t= 2.5s  ---      D0=H   bt_enable() complete + bt_le_adv_start()
t= 3–5s  634 µA   D0=H   Advertising — but 630 µA is WRONG, should be ~1 mA
                           → confirms i2c1/TWI was not yet disabled in this flash
t= 5.6s  ---      D0=L   bt_le_adv_stop() + bt_disable() + k_sleep()
t= 5.6–10.5s 634 µA D0=L bt_disable() had NO effect — MPSL still running
                           (DYNAMIC_INTERRUPTS chain was missing in this build)
t=10.5s 1138 µA   D0=L   LFRC calibration timer fires → mpsl_temperature_get()
                           blocks → HFCLK held → permanent 1.1 mA
```

This single CSV trace identified **three simultaneous problems** in one build:
TWI1 active, MPSL uninit broken, calibration timer blocking.

---

## Complete Fix Summary

### Kconfig (board .conf files)

```kconfig
# Disable all UART/console output — each holds a HFCLK request
CONFIG_USB_DEVICE_STACK=n   # XIAO only (not in board defconfig for DK)
CONFIG_SERIAL=n
CONFIG_CONSOLE=n
CONFIG_UART_CONSOLE=n
CONFIG_LOG=n

# Full MPSL uninit dependency chain — ALL FOUR required
# Kconfig silently ignores BT_UNINIT_MPSL_ON_DISABLE if any dep is missing
CONFIG_DYNAMIC_INTERRUPTS=y
CONFIG_DYNAMIC_DIRECT_INTERRUPTS=y
CONFIG_MPSL_DYNAMIC_INTERRUPTS=y
CONFIG_BT_UNINIT_MPSL_ON_DISABLE=y

# LFRC calibration calls mpsl_temperature_get() after mpsl_lib_uninit()
# → blocks indefinitely → HFCLK held permanently.  Disable it.
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION=n   # XIAO (RC clock source)
```

### Device Tree (board .overlay files)

```dts
/* QSPI driver holds HFINT even without a physical flash chip */
&qspi { status = "disabled"; };

/* nrfx TWI driver holds HFCLK from init() — disable unused I2C buses */
&i2c1 { status = "disabled"; };

/* Microphone power rail — XIAO Sense only */
/ {
    mic-vdd-off {
        compatible = "regulator-fixed";
        regulator-name = "MIC_PWR_OFF";
        enable-gpios = <&gpio1 10 GPIO_ACTIVE_HIGH>;
        regulator-always-off;
    };
};
```

### Application Code Pattern

```c
// ── Burst advertising cycle ───────────────────────────────────────────────
bt_enable(NULL);                            // re-init MPSL + SDC (~50 ms)
gpio_pin_set_dt(&active_ind, 1);            // D0 HIGH → PPK2 marker

bt_le_adv_start(&adv_param, ad, 3, NULL, 0);
k_sleep(K_SECONDS(3));                      // advertising burst
bt_le_adv_stop();

// bt_disable() calls hci_driver_close() → sdc_disable() → mpsl_lib_uninit()
// ONLY if the full Kconfig chain above is satisfied.
bt_disable();

gpio_pin_set_dt(&active_ind, 0);            // D0 LOW → sleep phase visible
k_sleep(K_SECONDS(27));                     // System ON / CPU asleep ~28 µA
// → loop back to bt_enable()
```

---

## CONFIG_PM=y Note

`CONFIG_PM=y` is **not effective on nRF52** and should not be used.  The
`select HAS_PM` symbol does not appear in any Nordic nRF52 Kconfig file, so the
option is silently ignored.  The CPU already enters WFI via the Zephyr kernel
idle thread (`__WFI()`) whenever there is nothing to run — no explicit PM
configuration is needed for that.

`CONFIG_PM_DEVICE=y` is valid and enables `pm_device_action_run()` for
suspending peripheral drivers (I2C, IMU).

---

## Diagnostics Checklist

When idle current is unexpectedly high, check each item in this order:

1. **`build/zephyr/.config`** — are all four DYNAMIC/MPSL symbols `=y`?
2. **`west build` output** — any `warning: XXXX was assigned the value 'y' but
   got the value 'n'`?  That signals a missing Kconfig dependency.
3. **DT overlays** — is every unused peripheral node set to
   `status = "disabled"`?  Check `i2c1`, `qspi`, `spi2`, `pwm0` in the board's
   common `.dtsi`.
4. **PPK2 CSV** — does the current change at all when `bt_disable()` is called
   (D0 goes LOW)?  If not: MPSL uninit is broken.
5. **PPK2 CSV** — does current jump at a fixed interval after boot (4 s, 5 s)?
   → LFRC calibration timer or BLE host timeout firing.
6. **`nrfjprog --recover`** — if flashing fails with error -90 (access
   protection), the chip is APPROTECT-locked.  Use `west flash --recover`.

---

## Validated Measurement — nRF52840-DK (2026-05-04)

This section documents the first fully successful run of `samples/020_bthome_tut2`
on the nRF52840-DK with all eight root-cause fixes applied.  Home Assistant
received every BTHome advertisement at the configured 30 s interval.

### PPK2 Screenshot

![PPK2 Measurement nRF52840-DK](../images/2026-05-04_ppk-020-nRF52840-DK.png)

The recording covers **46.17 s** at 3.3 V supply.  The PPK2 statistics panel
reports:

| Metric | Value |
|---|---|
| Average current (full window) | **67.66 µA** |
| Peak current | 2.43 mA |
| Total charge | 3.12 mC |
| D0 logic rows | row 0 = `active_ind` (HIGH = MCU awake) |

### Phase Analysis (`ppk-20260504T182249.csv`, 4 618 samples @ ~10 ms)

The CSV spans **46.7 s** and captures two complete advertising bursts.
D0 edge timestamps (ignoring metastable 'X' states):

| Event | Timestamp | D0 |
|---|---|---|
| Boot start | 0 ms | LOW |
| ADV burst 1 start | 2 580 ms | HIGH |
| ADV burst 1 end | 5 630 ms | LOW |
| ADV burst 2 start | 32 630 ms | HIGH |
| ADV burst 2 end | 35 690 ms | LOW |

Derived timings:
- **ADV burst duration:** 3 050–3 060 ms (target: `ADV_BURST_SEC = 3 s` ✓)
- **Sleep duration:** 27 000 ms (= 30 s − 3 s ✓)
- **Full cycle:** 30 060 ms ≈ 30 s ✓

### Per-Phase Current

| Phase | Duration | Avg | Min | Max | D0 |
|---|---|---|---|---|---|
| Boot + ADV burst 1 | 0–5 630 ms | 335 µA | 0 µA | 2 427 µA | — / H |
| **Sleep 1** | 5 630–32 630 ms | **27.84 µA** | 27.05 µA | 77.62 µA | LOW |
| **ADV burst 2** | 32 630–35 690 ms | **120.7 µA** | 110.3 µA | 543.9 µA | HIGH |
| **Sleep 2** | 35 690–46 740 ms | **27.86 µA** | 27.05 µA | 62.1 µA | LOW |

> The first ADV burst shows a lower per-sample average because it immediately
> follows the large boot spike (boot transient inflates the Boot row, while the
> ADV interval itself is short relative to the samples in that window).  The
> second burst gives a clean steady-state figure of **120.7 µA** average during
> the 3.06 s advertising phase.

### Steady-State Cycle Budget

Using burst 2 and sleep 1 as representative steady-state phases:

$$I_\text{cycle} = \frac{I_\text{adv} \cdot t_\text{adv} + I_\text{sleep} \cdot t_\text{sleep}}{t_\text{adv} + t_\text{sleep}}$$

$$I_\text{cycle} = \frac{120.7\,\mu\text{A} \times 3060\,\text{ms} + 27.84\,\mu\text{A} \times 27000\,\text{ms}}{30060\,\text{ms}} \approx \mathbf{37.3\,\mu\text{A}}$$

| Metric | Value |
|---|---|
| Sleep current (steady state) | **27.8 µA** |
| ADV burst average | **120.7 µA** |
| **Steady-state cycle average** | **37.3 µA** |
| Charge per 30 s cycle | **1.12 mC** |
| PPK2 window average (incl. boot) | 67.7 µA |

The 37.3 µA steady-state average is above the original < 20 µA target; the
remaining gap is dominated by the 27.8 µA sleep floor (nRF52840 System ON idle
with RTC and RAM retention).  Reaching sub-20 µA would require either System OFF
with timed wakeup (losing RAM) or a lower-power variant such as the nRF52810.
For a coin-cell BTHome sensor the 37 µA figure is well within practical limits
(CR2032 ≈ 230 mAh → **~255 days** of battery life at this rate).

### Confirmation — 6-minute Recording (`ppk2-20260504T184917.ppk2`)

A longer 6.1-minute recording (36 601 samples, 13 ADV bursts, 12 complete
steady-state cycles) was analysed with `scripts/ppk_analysis.py` and confirms
the results above with higher statistical confidence:

| Metric | Short run (46 s) | Long run (6.1 min) |
|---|---|---|
| Sleep current | 27.84 µA | **27.7 µA** |
| ADV burst avg (steady state) | 120.7 µA | **~123 µA** |
| Steady-state cycle avg | 37.3 µA | **37.6 µA** |
| CR2032 runtime estimate | ~255 days | **~255 days** |
| 2× AA runtime estimate | — | **7.6 years** |
| 2× AAA runtime estimate | — | **3.6 years** |

The two recordings agree to within 1 %, confirming that the sleep current is
stable over time and that there are no periodic wake-ups beyond the intended
30 s advertising cycle.

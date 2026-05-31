# BThome PIR Sample (`100_bthome_pir`)

Detects motion via a PIR sensor connected to a GPIO interrupt and advertises
the state over BThome V2 BLE.  Compatible with Home Assistant and any
BThome-capable hub.

## State Machine

```
IDLE (motion=0, slow interval ~3 s)
  │
  ├─ PIR interrupt ──► BURST (motion=1, fast interval 50 ms, 2 s duration)
  │                         │
  └─────────────────────────┘ burst_timer expires → back to IDLE
```

## Supported Boards

| Board | `BOARD=` | PIR GPIO | Overlay |
|-------|----------|----------|---------|
| nRF52840-DK | `nrf52840dk_nrf52840` | P0.11 (Button 1 / SW1) | `boards/nrf52840dk_nrf52840.overlay` |
| Seeed XIAO nRF52840 | `xiao_ble` | P0.02 (D0) | `boards/xiao_ble.overlay` |
| Seeed XIAO nRF52840 Sense | `xiao_ble_sense` | P0.02 (D0) | `boards/xiao_ble_sense.overlay` |

## Build & Flash

### nRF52840-DK

```bash
# Build + Flash via J-Link
make 100-flash
```

### Seeed XIAO nRF52840 / nRF52840 Sense

Die XIAO-Boards verwenden den **Adafruit UF2-Bootloader**. Es gibt zwei Wege:

#### Option A — UF2 Drag-and-Drop (kein zusätzliches Tool nötig)

1. **Doppelklick auf RESET** am Board — die LED beginnt zu pulsen (Bootloader aktiv)
2. Das Board erscheint als USB-Massenspeicher (z. B. `XIAO-SENSE` bzw. `XIAO-BLE`)
3. UF2-Datei in das Laufwerk kopieren — das Board startet automatisch neu

```bash
# Build
make 100-build BOARD=xiao_ble_sense

# UF2-Datei liegt nach dem Build unter:
#   build/zephyr/zephyr.uf2

# Linux-Beispiel (Einhängepunkt anpassen):
cp build/zephyr/zephyr.uf2 /media/$USER/XIAO-SENSE/
```

#### Option B — `make 100-flash` via adafruit-nrfutil

```bash
# Tool einmalig installieren:
uv tool install adafruit-nrfutil

# Board in DFU-Modus versetzen (Doppelklick RESET), dann:
make 100-flash BOARD=xiao_ble_sense

# Falls der DFU-Port nicht automatisch erkannt wird:
make 100-flash BOARD=xiao_ble_sense XIAO_DFU_PORT=/dev/ttyACM1
```

## Verify with bthome-logger

```bash
uv tool install bthome-logger
bthome-logger -f "MAKE"
```

---

## Important: BThome Device Header — Regular vs. Trigger-based

### The Header Byte

Every BThome V2 advertisement contains a **Device Information byte** immediately
after the UUID.  The two relevant values are:

| Value | Meaning | When to use |
|-------|---------|-------------|
| `0x40` | **Regular device** — reports an ongoing state | PIR, door/window contact, temperature, humidity, … |
| `0x44` | **Trigger-based device** — reports a momentary event | Button press, dimmer rotation, … |

Bit 2 (`0x04`) is the *trigger-based* flag.  `0x40 | 0x04 = 0x44`.

### Why PIR Must Use `0x40` (Regular Device)

A PIR sensor reports a **persistent state**: motion is either active or not.
Home Assistant expects `motion` (`0x21`) and similar binary sensors to come from
a **Regular device**.  If the Trigger-based flag is set, Home Assistant silently
ignores the advertisement because a transient event payload with a persistent
state object is considered invalid by the BThome specification.

**Rule of thumb:**

> Use **Regular device** (`0x40`) for any sensor that has a state which *stays*
> until something changes: motion, door open/closed, light on/off, temperature.
>
> Use **Trigger-based device** (`0x44`) only for truly momentary events that
> carry no lasting state: button press, single knob click, doorbell ring.

### In Code (`bthome_v2_init`)

```c
/* Regular device — correct for PIR / door / window sensors */
bthome_v2_init(&bthome, false, false);   /* → header 0x40 */

/* Trigger-based device — only for button-press style events */
bthome_v2_init(&bthome, false, true);    /* → header 0x44 */
```

The second `bool` parameter maps directly to the `trigger_based` field, which
sets `BTHOME_V2_DEV_INFO_TRIGGER` (bit 2) in the header byte.

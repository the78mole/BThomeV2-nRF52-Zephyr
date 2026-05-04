# 000_blinky

Minimal LED blink sample for the BThomeV2-nRF52-Zephyr workspace.  
Serves as a quick smoke-test to verify that the toolchain, board definition,
and flashing pipeline are all working before moving to more complex samples.

## Behaviour

LED0 blinks in a 1 s cycle: **100 ms ON / 900 ms OFF**.  
The LED is resolved via the board-agnostic `led0` Device Tree alias, so the
same firmware binary works on all supported boards without code changes.

## Supported boards

| Board | `BOARD` value |
|---|---|
| nRF52840-DK | `nrf52840dk/nrf52840` |
| Seeed XIAO nRF52840 (Sense / Plus) | `seeed_xiao_ble/nrf52840` |

## Build & flash

```bash
# nRF52840-DK
west build -b nrf52840dk/nrf52840   samples/000_blinky
west flash

# Seeed XIAO nRF52840
west build -b seeed_xiao_ble/nrf52840 samples/000_blinky
west flash
```

## Files

```
000_blinky/
├── CMakeLists.txt
├── prj.conf                  # CONFIG_GPIO=y
├── src/
│   └── main.c
└── boards/
    ├── nrf52840dk_nrf52840.overlay
    └── seeed_xiao_ble.overlay
```

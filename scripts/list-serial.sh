#!/usr/bin/env bash
# list-serial.sh — List all ttyACM / ttyUSB ports with USB device details.
#
# Usage:
#   ./scripts/list-serial.sh            # all serial ports
#   ./scripts/list-serial.sh --dfu      # only DFU / bootloader devices
#   ./scripts/list-serial.sh --monitor  # only "normal" firmware devices

set -uo pipefail

# ── Colour helpers ────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    BOLD='\033[1m'; CYAN='\033[0;36m'; GREEN='\033[0;32m'
    YELLOW='\033[0;33m'; RESET='\033[0m'
else
    BOLD=''; CYAN=''; GREEN=''; YELLOW=''; RESET=''
fi

# ── Known VID:PID tags ────────────────────────────────────────────────────────
declare -A KNOWN_DEVICES=(
    # Nordic
    ["1915:0101"]="nRF52-DK (J-Link CDC)"
    ["1915:c00a"]="Nordic nRF Connect USB / PPK2"
    # Adafruit / XIAO UF2 bootloader
    ["239a:0029"]="Adafruit nRF52 Bootloader (DFU)"
    ["239a:002a"]="Adafruit nRF52 Bootloader (DFU)"
    # Seeed XIAO nRF52840 — running Zephyr firmware
    ["2fe3:0100"]="XIAO nRF52840 (Zephyr USB-CDC)"
    # Seeed XIAO DFU bootloader
    ["2886:0045"]="XIAO nRF52840 DFU Bootloader"
    ["2886:0046"]="XIAO nRF52840 Sense DFU Bootloader"
    # Silicon Labs (common UART bridge)
    ["10c4:ea60"]="CP210x UART Bridge"
    # FTDI
    ["0403:6001"]="FTDI FT232 UART"
    ["0403:6010"]="FTDI FT2232 Dual UART"
    # Microchip / Atmel DFU
    ["03eb:2404"]="Atmel/SAMD DFU Bootloader"
)

# ── Filter flag ───────────────────────────────────────────────────────────────
FILTER=""
case "${1:-}" in
    --dfu)     FILTER="dfu" ;;
    --monitor) FILTER="monitor" ;;
esac

# ── Header ────────────────────────────────────────────────────────────────────
printf "${BOLD}%-12s  %-9s  %-28s  %-20s  %-28s  %s${RESET}\n" \
    "PORT" "VID:PID" "MANUFACTURER" "PRODUCT" "SERIAL" "NOTE"
printf '%0.s─' {1..120}; echo

# ── Main loop ─────────────────────────────────────────────────────────────────
found=0

for port in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -c "$port" ]] || continue

    dev=$(basename "$port")
    # ttyACM: device/.. is the USB interface parent (has idVendor etc.)
    # ttyUSB: device/../../../ is the USB device node
    if [[ "$dev" == ttyACM* ]]; then
        sysbase="/sys/class/tty/$dev/device/.."
    else
        sysbase="/sys/class/tty/$dev/device/../../.."
    fi
    [[ -d "$sysbase" ]] || sysbase="/sys/class/tty/$dev/device/.."

    vendor=$(cat "$sysbase/idVendor"    2>/dev/null || echo "????")
    product=$(cat "$sysbase/idProduct"  2>/dev/null || echo "????")
    mfg=$(cat    "$sysbase/manufacturer" 2>/dev/null | tr -d '\n' || echo "")
    prod=$(cat   "$sysbase/product"      2>/dev/null | tr -d '\n' || echo "")
    serial=$(cat "$sysbase/serial"       2>/dev/null | tr -d '\n' || echo "")
    vidpid="${vendor}:${product}"

    note="${KNOWN_DEVICES[$vidpid]:-}"

    # ── DFU / bootloader detection ────────────────────────────────────────────
    is_dfu=0
    if [[ "$note" == *"DFU"* || "$note" == *"Bootloader"* ]]; then
        is_dfu=1
    fi
    # XIAO running Zephyr firmware USB-CDC is not a DFU target
    [[ "$note" == *"Zephyr"* ]] && is_dfu=0

    # ── Apply filter ──────────────────────────────────────────────────────────
    if [[ "$FILTER" == "dfu"     && $is_dfu -eq 0 ]]; then continue; fi
    if [[ "$FILTER" == "monitor" && $is_dfu -eq 1 ]]; then continue; fi

    # ── Colour selection ──────────────────────────────────────────────────────
    if   [[ $is_dfu -eq 1 ]];         then colour="$YELLOW"
    elif [[ -n "$note" ]];             then colour="$GREEN"
    else                                    colour="$CYAN"
    fi

    printf "${colour}%-12s  %-9s  %-28s  %-20s  %-28s  %s${RESET}\n" \
        "$port" "$vidpid" "${mfg:0:27}" "${prod:0:19}" "${serial:0:27}" "$note"
    found=$((found + 1))
done

echo
if [[ $found -eq 0 ]]; then
    echo "  No serial ports found${FILTER:+ matching filter '--$FILTER'}."
else
    printf "  ${BOLD}%d port(s) listed${RESET}\n" "$found"
fi

# ── Quick hint for XIAO DFU ───────────────────────────────────────────────────
if [[ "$FILTER" != "dfu" ]]; then
    xiao_dfu=$(for p in /dev/ttyACM* /dev/ttyUSB*; do
        [[ -e "$p" ]] || continue
        dev=$(basename "$p")
        vid=$(cat "/sys/class/tty/$dev/device/../idVendor"  2>/dev/null || true)
        pid=$(cat "/sys/class/tty/$dev/device/../idProduct" 2>/dev/null || true)
        key="${vid}:${pid}"
        note="${KNOWN_DEVICES[$key]:-}"
        [[ "$note" == *"DFU"* || "$note" == *"Bootloader"* ]] && echo "$p"
    done | head -1)

    if [[ -n "$xiao_dfu" ]]; then
        echo
        printf "  ${YELLOW}${BOLD}DFU target detected:${RESET}  make <NNN>-flash BOARD=xiao_ble_sense XIAO_DFU_PORT=%s\n" "$xiao_dfu"
    fi
fi
echo

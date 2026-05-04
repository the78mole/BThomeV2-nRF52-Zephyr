#!/usr/bin/env bash
# find-port.sh — Return the serial port path for a given USB VID:PID.
#
# Usage:
#   find-port.sh [OPTIONS] VID:PID [VID:PID ...]
#
# Options:
#   --product <substr>   Only match if the USB product string contains substr
#                        (case-insensitive substring match)
#   --index <N>          Return the Nth match (0-based, default: 0)
#   --quiet              Suppress error messages on stderr
#
# Output:
#   Prints the matching port path (e.g. /dev/ttyACM0) to stdout.
#
# Exit codes:
#   0  Port found
#   1  No matching port found
#
# Examples:
#   find-port.sh 1915:c00a                          # nRF52840-DK (first port)
#   find-port.sh --product Connectivity 1915:c00a   # nRF52840-DK UART bridge
#   find-port.sh 2886:0045 2886:0046                # XIAO DFU bootloader
#   find-port.sh --index 1 1915:c00a                # second matching port

set -uo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────────
PRODUCT_FILTER=""
INDEX=0
QUIET=0
VIDPIDS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --product) PRODUCT_FILTER="$2"; shift 2 ;;
        --index)   INDEX="$2";          shift 2 ;;
        --quiet)   QUIET=1;             shift   ;;
        -*)        echo "Unknown option: $1" >&2; exit 1 ;;
        *)         VIDPIDS+=("$1");     shift   ;;
    esac
done

if [[ ${#VIDPIDS[@]} -eq 0 ]]; then
    echo "Usage: find-port.sh [--product <substr>] [--index <N>] VID:PID [VID:PID ...]" >&2
    exit 1
fi

# ── Search ────────────────────────────────────────────────────────────────────
match_count=0

for port in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -c "$port" ]] || continue

    dev=$(basename "$port")

    # Resolve sysfs path: ttyACM → interface parent, ttyUSB → device parent
    if [[ "$dev" == ttyACM* ]]; then
        sysbase="/sys/class/tty/$dev/device/.."
    else
        sysbase="/sys/class/tty/$dev/device/../../.."
    fi
    [[ -d "$sysbase" ]] || continue

    vendor=$(cat  "$sysbase/idVendor"  2>/dev/null | tr -d '[:space:]' || true)
    product_id=$(cat "$sysbase/idProduct" 2>/dev/null | tr -d '[:space:]' || true)
    [[ -n "$vendor" && -n "$product_id" ]] || continue

    vidpid="${vendor}:${product_id}"

    # ── VID:PID match ─────────────────────────────────────────────────────────
    matched_vidpid=0
    for want in "${VIDPIDS[@]}"; do
        if [[ "${vidpid,,}" == "${want,,}" ]]; then
            matched_vidpid=1
            break
        fi
    done
    [[ $matched_vidpid -eq 1 ]] || continue

    # ── Optional product string filter ────────────────────────────────────────
    if [[ -n "$PRODUCT_FILTER" ]]; then
        prod_str=$(cat "$sysbase/product" 2>/dev/null | tr -d '\n' || true)
        # Case-insensitive substring match
        if [[ "${prod_str,,}" != *"${PRODUCT_FILTER,,}"* ]]; then
            continue
        fi
    fi

    # ── Index selection ───────────────────────────────────────────────────────
    if [[ $match_count -eq $INDEX ]]; then
        echo "$port"
        exit 0
    fi
    match_count=$((match_count + 1))
done

[[ $QUIET -eq 1 ]] || \
    echo "find-port: no port found for VID:PID [${VIDPIDS[*]}]${PRODUCT_FILTER:+ product='$PRODUCT_FILTER'}" >&2
exit 1

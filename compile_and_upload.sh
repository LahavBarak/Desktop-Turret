#!/usr/bin/env bash
# compile_and_upload.sh — Auto-detect ESP32 board and compile/upload a sketch
# Usage: ./compile_and_upload.sh <path/to/sketch.ino> [--board c3|p4|C3|P4]

set -euo pipefail

SKETCH_PATH=""
OPT_BOARD=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --board) OPT_BOARD="${2^^}"; shift 2 ;;  # normalize to uppercase: c3->C3, p4->P4
        *)       SKETCH_PATH="$1"; shift ;;
    esac
done

if [[ -z "$SKETCH_PATH" ]]; then
    echo "Usage: $0 <sketch-path> [--board c3|p4]" >&2
    exit 1
fi

# Resolve absolute path
SKETCH_PATH="$(realpath "$SKETCH_PATH")"

# Board definitions: chip name -> FQBN
declare -A BOARDS
BOARDS["ESP32-C3"]="esp32:esp32:XIAO_ESP32C3:CDCOnBoot=default"
BOARDS["ESP32-P4"]="esp32:esp32:esp32p4"

# VID:PID -> board name (matches arduino-cli's 0xXXXX:0xXXXX format)
declare -A VIDPID
VIDPID["0x303a:0x1001"]="ESP32-C3"   # Espressif USB JTAG/serial (XIAO C3)
VIDPID["0x1a86:0x55d3"]="ESP32-P4"   # CH343 USB-serial on P4 Nano

# ── Auto-detect board ─────────────────────────────────────────────────────────
PORT=""
CHIP=""
BOARD_NAME=""

# If --board specified, resolve it to a BOARDS key (ESP32-C3 or ESP32-P4)
if [[ -n "$OPT_BOARD" ]]; then
    for key in "${!BOARDS[@]}"; do
        if [[ "$key" == *"$OPT_BOARD"* ]]; then
            CHIP="$key"
            break
        fi
    done
    if [[ -z "$CHIP" ]]; then
        echo -e "\033[31mUnknown board '$OPT_BOARD'. Valid options: c3, p4\033[0m" >&2
        exit 1
    fi
fi

echo -e "\033[36mScanning for ESP32 board...\033[0m"

JSON=$(arduino-cli board list --format json 2>/dev/null)
PORT_COUNT=$(echo "$JSON" | jq '(.detected_ports // .) | length')

for (( idx=0; idx<PORT_COUNT; idx++ )); do
    ENTRY=$(echo "$JSON" | jq -c "(.detected_ports // .)[$idx]")
    PORT_ADDR=$(echo "$ENTRY" | jq -r '.port.address')

    # Method 1: matching_boards (arduino-cli identified the board)
    MATCH_COUNT=$(echo "$ENTRY" | jq '.matching_boards | length // 0')
    for (( b=0; b<MATCH_COUNT; b++ )); do
        BOARD_FQBN=$(echo "$ENTRY" | jq -r ".matching_boards[$b].fqbn")
        BOARD_NAME_RAW=$(echo "$ENTRY" | jq -r ".matching_boards[$b].name")
        for chip in "${!BOARDS[@]}"; do
            [[ -n "$OPT_BOARD" && "$chip" != *"$OPT_BOARD"* ]] && continue
            chip_lower="${chip,,}"
            chip_nohyphen="${chip_lower//-/}"
            if [[ "$BOARD_FQBN" == *"$chip_nohyphen"* || "$BOARD_NAME_RAW" == *"$chip"* ]]; then
                PORT="$PORT_ADDR"
                CHIP="$chip"
                BOARD_NAME="$BOARD_NAME_RAW"
                break 2
            fi
        done
    done

    # Method 2: VID:PID fallback
    VID=$(echo "$ENTRY" | jq -r '.port.properties.vid // empty')
    PID=$(echo "$ENTRY" | jq -r '.port.properties.pid // empty')
    if [[ -n "$VID" && -n "$PID" ]]; then
        KEY="${VID,,}:${PID,,}"
        if [[ -n "${VIDPID[$KEY]:-}" ]]; then
            DETECTED_CHIP="${VIDPID[$KEY]}"
            if [[ -z "$OPT_BOARD" || "$DETECTED_CHIP" == *"$OPT_BOARD"* ]]; then
                PORT="$PORT_ADDR"
                CHIP="$DETECTED_CHIP"
                BOARD_NAME="$CHIP (detected via USB VID:PID)"
            fi
        fi
    fi

    [[ -n "$PORT" ]] && break
done

if [[ -z "$PORT" ]]; then
    if [[ -n "$OPT_BOARD" ]]; then
        echo -e "\033[31mESP32-$OPT_BOARD not found. Make sure it's plugged in.\033[0m" >&2
    else
        echo -e "\033[31mNo supported ESP32 board found. Make sure it's plugged in.\033[0m" >&2
        echo "Supported boards: ${!BOARDS[*]}" >&2
    fi
    exit 1
fi

FQBN="${BOARDS[$CHIP]}"

echo ""
echo -e "\033[36m=== Compiling & Uploading ===\033[0m"
echo "Board:  $BOARD_NAME ($CHIP)"
echo "FQBN:   $FQBN"
echo "Port:   $PORT"
echo "Sketch: $SKETCH_PATH"
echo ""

# ── Compile + upload ──────────────────────────────────────────────────────────
arduino-cli compile --fqbn "$FQBN" -u -p "$PORT" "$SKETCH_PATH"
EXIT_CODE=$?

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    echo -e "\033[32mDone! Upload successful.\033[0m"
else
    echo -e "\033[31mFailed with exit code $EXIT_CODE\033[0m"
    exit $EXIT_CODE
fi

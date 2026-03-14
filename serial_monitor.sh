#!/usr/bin/env bash
# serial_monitor.sh — Auto-detect ESP32 board and open a serial monitor
# Usage:
#   ./serial_monitor.sh                  # auto-detect board
#   ./serial_monitor.sh --port /dev/ttyACM0
#   ./serial_monitor.sh --baud 2000000   # override baud (default: 115200 C3, 2000000 P4)
#   ./serial_monitor.sh --board P4       # force P4 when both boards connected
# Ctrl+A Ctrl+X to exit picocom.

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────
OPT_PORT=""
OPT_BAUD=""
OPT_BOARD=""

declare -A VIDPID
VIDPID["0x303a:0x1001"]="ESP32-C3"
VIDPID["0x1a86:0x55d3"]="ESP32-P4"

declare -A DEFAULT_BAUD
DEFAULT_BAUD["ESP32-C3"]=115200
DEFAULT_BAUD["ESP32-P4"]=115200

# ── Argument parsing ───────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)  OPT_PORT="$2";  shift 2 ;;
        --baud)  OPT_BAUD="$2";  shift 2 ;;
        --board) OPT_BOARD="${2^^}"; shift 2 ;;  # uppercase
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Board detection ────────────────────────────────────────────────────────────
detect_boards() {
    local json port_count idx entry port_addr protocol vid pid key name
    json=$(arduino-cli board list --format json 2>/dev/null)
    port_count=$(echo "$json" | jq '(.detected_ports // .) | length')

    for (( idx=0; idx<port_count; idx++ )); do
        entry=$(echo "$json" | jq -c "(.detected_ports // .)[$idx]")
        protocol=$(echo "$entry" | jq -r '.port.protocol_label // empty')
        # Skip non-USB ports (e.g. /dev/ttyS0)
        [[ "$protocol" == *USB* ]] || continue

        port_addr=$(echo "$entry" | jq -r '.port.address')

        # Method 1: matching_boards
        name=$(echo "$entry" | jq -r '.matching_boards[0].name // empty')

        # Method 2: VID:PID fallback
        if [[ -z "$name" ]]; then
            vid=$(echo "$entry" | jq -r '.port.properties.vid // empty')
            pid=$(echo "$entry" | jq -r '.port.properties.pid // empty')
            if [[ -n "$vid" && -n "$pid" ]]; then
                key="${vid,,}:${pid,,}"
                name="${VIDPID[$key]:-}"
            fi
        fi

        [[ -z "$name" ]] && name="ESP32 (USB device)"
        echo "$port_addr $name"
    done
}

if [[ -n "$OPT_PORT" ]]; then
    PORT="$OPT_PORT"
    # Try to identify board name for default baud
    BOARD_NAME=""
    while IFS=" " read -r p b; do
        [[ "$p" == "$PORT" ]] && BOARD_NAME="$b"
    done < <(detect_boards)
else
    mapfile -t BOARDS < <(detect_boards)
    if [[ ${#BOARDS[@]} -eq 0 ]]; then
        echo -e "\033[31mNo supported ESP32 board found. Make sure it's plugged in.\033[0m" >&2
        exit 1
    fi

    if [[ -n "$OPT_BOARD" ]]; then
        MATCH=""
        for entry in "${BOARDS[@]}"; do
            read -r p b <<< "$entry"
            if [[ "$b" == *"$OPT_BOARD"* ]]; then
                MATCH="$entry"; break
            fi
        done
        if [[ -z "$MATCH" ]]; then
            echo "Board '$OPT_BOARD' not found. Detected:" >&2
            printf '  %s\n' "${BOARDS[@]}" >&2
            exit 1
        fi
        read -r PORT BOARD_NAME <<< "$MATCH"
    elif [[ ${#BOARDS[@]} -eq 1 ]]; then
        read -r PORT BOARD_NAME <<< "${BOARDS[0]}"
    else
        echo "Multiple boards detected:"
        for i in "${!BOARDS[@]}"; do
            echo "  [$i] ${BOARDS[$i]}"
        done
        # Read from /dev/tty so this works even under `sg dialout -c`
        read -rp "Select board number: " CHOICE </dev/tty
        read -r PORT BOARD_NAME <<< "${BOARDS[$CHOICE]}"
    fi
fi

BAUD="${OPT_BAUD:-${DEFAULT_BAUD[$BOARD_NAME]:-115200}}"

# ── Connect ────────────────────────────────────────────────────────────────────
echo -e "\033[36mBoard: ${BOARD_NAME:-unknown}\033[0m"
echo -e "\033[36mPort:  $PORT\033[0m"
echo -e "\033[36mBaud:  $BAUD\033[0m"
echo -e "\033[32mOpening monitor. Ctrl+C to exit.\033[0m"
echo ""

trap 'exit 0' INT

set +e
while true; do
    while [[ ! -e "$PORT" ]]; do
        sleep 0.2
    done

    arduino-cli monitor -p "$PORT" -c "baudrate=$BAUD"

    # Board disconnected → reconnect
    echo -e "\033[33m--- board disconnected, reconnecting... ---\033[0m"
    sleep 1
done

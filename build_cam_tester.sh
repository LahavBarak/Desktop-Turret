#!/usr/bin/env bash
# build_cam_tester.sh — Build and optionally flash/monitor the cam_tester IDF project
# Usage:
#   ./build_cam_tester.sh              # build only
#   ./build_cam_tester.sh flash        # build + flash (auto-detects P4 port)
#   ./build_cam_tester.sh monitor      # build + flash + serial monitor

set -euo pipefail

ACTION="${1:-build}"
IDF_PATH="${HOME}/esp/esp-idf"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/cam_tester"

# ── Source ESP-IDF environment ─────────────────────────────────────────────────
if [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    echo "ESP-IDF not found at ${IDF_PATH}" >&2
    echo "Run: git clone -b v5.4.1 --depth 1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf && ~/esp/esp-idf/install.sh esp32p4" >&2
    exit 1
fi

echo -e "\033[36mSourcing ESP-IDF environment...\033[0m"
# shellcheck disable=SC1091
source "${IDF_PATH}/export.sh" 2>&1 | grep -v "^$" | grep -v "^Detecting" | true

# ── Auto-detect P4 port ────────────────────────────────────────────────────────
PORT=""
for DEV in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$DEV" ]] || continue
    VID=$(udevadm info -q property "$DEV" 2>/dev/null | awk -F= '/^ID_VENDOR_ID=/{print tolower($2)}')
    PID=$(udevadm info -q property "$DEV" 2>/dev/null | awk -F= '/^ID_MODEL_ID=/{print tolower($2)}')
    if [[ "$VID:$PID" == "1a86:55d3" ]]; then
        PORT="$DEV"
        break
    fi
done

echo ""
echo -e "\033[36m=== ESP32-P4 Cam Tester ===\033[0m"
echo "Project: $PROJECT_DIR"
[[ -n "$PORT" ]] && echo "Port:    $PORT" || echo "Port:    (not detected — flash/monitor unavailable)"
echo ""

cd "$PROJECT_DIR"

case "$ACTION" in
    build)
        idf.py set-target esp32p4 2>/dev/null || true
        idf.py build
        ;;
    flash)
        if [[ -z "$PORT" ]]; then
            echo -e "\033[31mESP32-P4-Nano not found. Make sure it's plugged in.\033[0m" >&2
            exit 1
        fi
        idf.py set-target esp32p4 2>/dev/null || true
        idf.py -p "$PORT" build flash
        ;;
    monitor)
        if [[ -z "$PORT" ]]; then
            echo -e "\033[31mESP32-P4-Nano not found. Make sure it's plugged in.\033[0m" >&2
            exit 1
        fi
        idf.py set-target esp32p4 2>/dev/null || true
        idf.py -p "$PORT" build flash monitor
        ;;
    *)
        echo "Unknown action: $ACTION. Use: build | flash | monitor" >&2
        exit 1
        ;;
esac

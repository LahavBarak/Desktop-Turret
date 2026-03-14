#!/usr/bin/env python3
"""
serial_monitor.py — Auto-detect ESP32 board, open serial monitor, auto-reconnect on reset.
Ctrl+C to exit.

Usage:
    python3 serial_monitor.py                  # auto-detect
    python3 serial_monitor.py --board P4
    python3 serial_monitor.py --port /dev/ttyACM0 --baud 115200
"""

import argparse
import select
import sys
import termios
import threading
import time
import tty

import serial
import serial.tools.list_ports

VIDPID_MAP = {
    (0x303A, 0x1001): ("ESP32-C3", 115200),
    (0x1A86, 0x55D3): ("ESP32-P4", 2000000),
}


def detect_boards():
    found = []
    for p in serial.tools.list_ports.comports():
        key = (p.vid, p.pid)
        if key in VIDPID_MAP:
            name, default_baud = VIDPID_MAP[key]
            found.append((p.device, name, default_baud, p.vid, p.pid))
    return found


def wait_for_board(vid, pid, stop_event):
    """Wait until VID:PID reappears (handles ttyACMx number change after reset)."""
    while not stop_event.is_set():
        for p in serial.tools.list_ports.comports():
            if p.vid == vid and p.pid == pid:
                return p.device
        time.sleep(0.2)
    return None


def run_session(port_path, baud, stop_event):
    """
    Open port and stream data until disconnected or user exits.
    Returns True if the user requested exit (Ctrl+C), False if the board disconnected.

    Key: dsrdtr=False, rtscts=False — pyserial opens the port without asserting
    DTR or RTS. Linux's cdc-acm driver asserts both by default on open; RTS is
    wired to the ESP32-C3's EN (reset) pin, so leaving it high causes an
    immediate reset. pyserial can prevent this because it controls the TIOCM
    ioctl before the ACM driver's SET_CONTROL_LINE_STATE USB request completes.
    """
    try:
        ser = serial.Serial(
            port_path, baud,
            timeout=0.1,
            dsrdtr=True,    # assert DTR → ESP32 HWCDC sets connected=true → Serial.print() works
            rtscts=False,   # don't assert RTS → RTS drives the EN/reset pin → asserting it resets the board
        )
    except serial.SerialException:
        return False

    disconnected = threading.Event()

    def reader():
        while not disconnected.is_set() and not stop_event.is_set():
            try:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
            except (serial.SerialException, OSError):
                disconnected.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    user_exit = False
    try:
        tty.setraw(fd)
        while not disconnected.is_set() and not stop_event.is_set():
            r, _, _ = select.select([sys.stdin], [], [], 0.2)
            if not r:
                continue
            ch = sys.stdin.buffer.read(1)
            if not ch or ch in (b'\x03', b'\x04'):  # Ctrl+C / Ctrl+D
                stop_event.set()
                user_exit = True
                break
            if ch == b'\r':
                ch = b'\n'
            try:
                ser.write(ch)
            except (serial.SerialException, OSError):
                disconnected.set()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        ser.close()
        t.join(timeout=1)

    return user_exit


def main():
    parser = argparse.ArgumentParser(description="ESP32 serial monitor")
    parser.add_argument("--port",  help="Serial port (skips auto-detect)")
    parser.add_argument("--baud",  type=int, help="Baud rate override")
    parser.add_argument("--board", help="Force board: C3 or P4")
    args = parser.parse_args()

    boards = detect_boards()
    vid, pid = None, None

    if args.port:
        port_path = args.port
        board_name = "unknown"
        baud = args.baud or 115200
        for p in serial.tools.list_ports.comports():
            if p.device == args.port and (p.vid, p.pid) in VIDPID_MAP:
                vid, pid = p.vid, p.pid
                board_name, baud = VIDPID_MAP[(vid, pid)]
                break
        if args.baud:
            baud = args.baud
    else:
        if not boards:
            print("\033[31mNo supported ESP32 board found. Make sure it's plugged in.\033[0m",
                  file=sys.stderr)
            sys.exit(1)

        if args.board:
            matches = [b for b in boards if args.board.upper() in b[1].upper()]
            if not matches:
                names = [f"{n} ({p})" for p, n, *_ in boards]
                print(f"Board '{args.board}' not found. Detected: {names}", file=sys.stderr)
                sys.exit(1)
            port_path, board_name, baud, vid, pid = matches[0]
        elif len(boards) == 1:
            port_path, board_name, baud, vid, pid = boards[0]
        else:
            print("Multiple boards detected:")
            for i, (p, n, b, *_) in enumerate(boards):
                print(f"  [{i}] {n}  ({p})")
            choice = int(input("Select board number: "))
            port_path, board_name, baud, vid, pid = boards[choice]

        if args.baud:
            baud = args.baud

    print(f"\033[36mBoard: {board_name}\033[0m")
    print(f"\033[36mPort:  {port_path}\033[0m")
    print(f"\033[36mBaud:  {baud}\033[0m")
    print(f"\033[32mConnected. Ctrl+C to exit.\033[0m\n")

    stop_event = threading.Event()

    while not stop_event.is_set():
        user_exit = run_session(port_path, baud, stop_event)
        if user_exit or stop_event.is_set():
            break

        print("\033[33m--- board disconnected, reconnecting... ---\033[0m")

        if vid and pid:
            # Re-detect by VID:PID — port number may have changed after reset
            new_port = wait_for_board(vid, pid, stop_event)
            if new_port:
                port_path = new_port
        else:
            time.sleep(2)

    print("\n\033[36mDisconnected.\033[0m")


if __name__ == "__main__":
    main()

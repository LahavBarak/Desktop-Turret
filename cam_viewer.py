"""
ESP32-P4 Camera Viewer
Receives JPEG frames from cam_tester over USB serial and displays them live.

Usage:
    python cam_viewer.py                  # auto-detect port, 2 Mbaud
    python cam_viewer.py /dev/ttyUSB0     # explicit port
    python cam_viewer.py /dev/ttyUSB0 -v  # verbose (prints per-frame debug)

Controls:
    Q / ESC  — quit
    S        — save screenshot
"""

import sys
import time
import struct
import serial
import serial.tools.list_ports
import numpy as np
import cv2

FRAME_MAGIC  = b'\xAA\x55\xAA\x55'
FRAME_FOOTER = b'\xFF\xD9'
BAUD         = 2_000_000
MAX_FRAME    = 300 * 1024   # 300 KB hard cap; anything larger is a sync error

VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv


# ── port detection ──────────────────────────────────────────────────────────

def find_port():
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        if any(k in desc for k in ("ch343", "ch340", "esp", "uart", "usb serial")):
            if p.device not in ("COM1", "/dev/tty.Bluetooth-Incoming-Port"):
                return p.device
    # fallback: first port that isn't COM1
    for p in serial.tools.list_ports.comports():
        if p.device != "COM1":
            return p.device
    return None


# ── frame reception ──────────────────────────────────────────────────────────

def sync_to_header(ser):
    """
    Scan the byte stream for FRAME_MAGIC.
    Reads in 512-byte chunks for efficiency; falls back to byte-by-byte
    near a potential header boundary.
    Returns True when the magic is found (stream is now positioned right
    after the magic bytes), False on timeout.
    """
    CHUNK   = 512
    TIMEOUT = 10.0   # seconds before printing a diagnostic
    buf     = b''
    t0      = time.monotonic()

    while True:
        data = ser.read(CHUNK)
        if not data:
            if time.monotonic() - t0 > TIMEOUT:
                print("[sync] Still searching for frame header …", flush=True)
                t0 = time.monotonic()
            continue

        buf += data

        idx = buf.find(FRAME_MAGIC)
        if idx != -1:
            # Consume everything up to and including the magic.
            # Any leftover bytes after the magic belong to the next read.
            leftover = buf[idx + len(FRAME_MAGIC):]
            # Push leftover back by seeking; simpler: store in a wrapper.
            # We can't un-read from pyserial, but we can put it in a global
            # and drain it first — see _leftover below.
            _push_back(leftover)
            return True

        # Keep only the last (len(FRAME_MAGIC)-1) bytes in case the magic
        # straddles a chunk boundary.
        buf = buf[-(len(FRAME_MAGIC) - 1):]


_pending = bytearray()   # bytes "pushed back" after a sync overshoot

def _push_back(data: bytes):
    global _pending
    _pending = bytearray(data) + _pending

def _read_exact(ser, n):
    """Read exactly n bytes, draining _pending first."""
    global _pending
    out = bytearray()

    if _pending:
        take = min(n, len(_pending))
        out += _pending[:take]
        _pending = _pending[take:]
        n -= take

    while n > 0:
        chunk = ser.read(n)
        if not chunk:
            return None   # timeout
        out += chunk
        n -= len(chunk)

    return bytes(out)


def receive_frame(ser):
    """
    Synchronise to the next frame header, read the payload, verify the footer.
    Returns raw JPEG bytes on success, None on any error.
    """
    if not sync_to_header(ser):
        return None

    # Length field (4 bytes LE)
    lb = _read_exact(ser, 4)
    if lb is None:
        if VERBOSE: print("[frame] timeout reading length")
        return None

    size = struct.unpack('<I', lb)[0]
    if size == 0 or size > MAX_FRAME:
        if VERBOSE: print(f"[frame] invalid size {size}")
        return None

    # JPEG payload
    data = _read_exact(ser, size)
    if data is None:
        if VERBOSE: print(f"[frame] timeout reading {size} B payload")
        return None

    # Footer (0xFF 0xD9)
    footer = _read_exact(ser, 2)
    if footer != FRAME_FOOTER:
        if VERBOSE:
            print(f"[frame] footer mismatch: {footer.hex() if footer else 'None'}")
        return None

    return data


# ── main ────────────────────────────────────────────────────────────────────

def main():
    # Parse arguments (ignore flags like -v that were already consumed)
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    port = args[0] if args else None

    if not port:
        port = find_port()
    if not port:
        print("ERROR: no serial port found.  Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        sys.exit(1)

    print(f"Port:     {port}")
    print(f"Baudrate: {BAUD:,}")
    print(f"Verbose:  {'yes' if VERBOSE else 'no  (add -v to enable)'}")
    print(f"Controls: Q/ESC = quit,  S = screenshot")
    print()

    try:
        ser = serial.Serial(port, BAUD, timeout=2)
    except serial.SerialException as e:
        print(f"ERROR: cannot open {port}: {e}")
        sys.exit(1)

    print("Waiting for first frame …")

    cv2.namedWindow("ESP32-P4 Camera", cv2.WINDOW_NORMAL)

    frame_count  = 0
    drop_count   = 0
    fps          = 0.0
    fps_t0       = time.monotonic()
    fps_frames   = 0
    screenshot_n = 0

    try:
        while True:
            jpeg = receive_frame(ser)

            if jpeg is None:
                drop_count += 1
                if VERBOSE:
                    print(f"[drop] #{drop_count}", flush=True)
                continue

            arr   = np.frombuffer(jpeg, dtype=np.uint8)
            frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if frame is None:
                if VERBOSE:
                    print(f"[warn] imdecode failed ({len(jpeg)} B)", flush=True)
                continue

            frame_count += 1
            fps_frames  += 1

            now = time.monotonic()
            if now - fps_t0 >= 1.0:
                fps       = fps_frames / (now - fps_t0)
                fps_frames = 0
                fps_t0    = now

            # Overlay
            h, w = frame.shape[:2]
            label = f"FPS {fps:.1f}  |  {len(jpeg)//1024}.{(len(jpeg)%1024)*10//1024}KB  |  {w}x{h}"
            cv2.putText(frame, label, (10, 28),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2, cv2.LINE_AA)

            cv2.imshow("ESP32-P4 Camera", frame)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord('q'), 27):
                break
            if key == ord('s'):
                fname = f"screenshot_{screenshot_n:04d}.jpg"
                cv2.imwrite(fname, frame)
                print(f"Saved {fname}")
                screenshot_n += 1

    except KeyboardInterrupt:
        print()
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print(f"Done.  {frame_count} frames received, {drop_count} dropped.")


if __name__ == "__main__":
    main()

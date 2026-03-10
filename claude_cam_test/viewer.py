#!/usr/bin/env python3
"""
viewer.py — display JPEG frames streamed from the ESP32-P4 camera.

Wire protocol (must match main.c):
    [0xAA 0x55 0xAA 0x55]  magic header   (4 bytes)
    [length]               uint32 LE       (4 bytes)
    [JPEG payload]         <length> bytes
    [0xFF 0xD9]            footer          (2 bytes)

Usage:
    python viewer.py                        # auto-detect port
    python viewer.py /dev/ttyUSB0          # Linux explicit
    python viewer.py COM3                  # Windows explicit
    python viewer.py COM3 --verbose        # per-frame debug
    python viewer.py COM3 --save ./frames  # save frames to directory
"""

import argparse
import struct
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import serial
import serial.tools.list_ports

# ── Protocol constants (must match main.c) ────────────────────────────────────
MAGIC  = bytes([0xAA, 0x55, 0xAA, 0x55])
FOOTER = bytes([0xFF, 0xD9])
BAUD   = 2_000_000

MAX_FRAME_BYTES = 512 * 1024   # sanity cap — no JPEG should exceed 512 KB


# ── Serial helpers ─────────────────────────────────────────────────────────────

def auto_detect_port() -> str | None:
    """Return the first USB-serial port that looks like a CH343/CH340."""
    ports = list(serial.tools.list_ports.comports())
    # Preferred: CH343 / CH340 (the USB-serial chip on ESP32-P4 Nano)
    for p in ports:
        desc = (p.description or "").lower()
        if any(tok in desc for tok in ("ch343", "ch340", "uart", "usb serial")):
            return p.device
    # Fall back to the first available port
    return ports[0].device if ports else None


def read_exact(port: serial.Serial, n: int) -> bytes:
    """Read exactly *n* bytes, blocking until they arrive."""
    buf = bytearray()
    while len(buf) < n:
        chunk = port.read(n - len(buf))
        if not chunk:
            raise IOError("Serial port closed or timed out while reading")
        buf.extend(chunk)
    return bytes(buf)


def sync_to_magic(port: serial.Serial) -> None:
    """Consume bytes one-by-one until the 4-byte magic header is aligned."""
    window = bytearray(4)
    while True:
        b = port.read(1)
        if not b:
            continue
        window = window[1:] + bytearray(b)
        if bytes(window) == MAGIC:
            return


# ── Frame receiver ─────────────────────────────────────────────────────────────

def recv_frame(port: serial.Serial, verbose: bool) -> bytes | None:
    """
    Receive one frame.  Syncs to magic, reads length, payload, footer.
    Returns the raw JPEG bytes, or None if the frame is malformed.
    """
    sync_to_magic(port)

    length_bytes = read_exact(port, 4)
    length = struct.unpack("<I", length_bytes)[0]

    if length == 0 or length > MAX_FRAME_BYTES:
        if verbose:
            print(f"[WARN] Implausible frame length {length} — resyncing")
        return None

    payload = read_exact(port, length)

    footer = read_exact(port, 2)
    if footer != FOOTER:
        if verbose:
            print(f"[WARN] Footer mismatch: {footer.hex()} — frame dropped")
        return None

    return payload


# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-P4 OV5647 camera viewer")
    parser.add_argument("port", nargs="?",
                        help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print per-frame statistics")
    parser.add_argument("--save", metavar="DIR",
                        help="Save decoded frames as JPEG files to DIR")
    args = parser.parse_args()

    port_name = args.port or auto_detect_port()
    if not port_name:
        print("ERROR: No serial port found.  Pass the port as an argument.", file=sys.stderr)
        sys.exit(1)

    save_dir: Path | None = None
    if args.save:
        save_dir = Path(args.save)
        save_dir.mkdir(parents=True, exist_ok=True)
        print(f"Saving frames to {save_dir}/")

    print(f"Connecting to {port_name} @ {BAUD} baud …")
    port = serial.Serial(port_name, BAUD, timeout=3.0)
    print("Connected.  Waiting for first frame (press Ctrl-C to quit) …\n")

    cv2.namedWindow("ESP32-P4 Camera", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("ESP32-P4 Camera", 800, 640)

    total_frames = 0
    fps_frames   = 0
    fps          = 0.0
    t_fps        = time.perf_counter()
    save_idx     = 0

    try:
        while True:
            jpeg = recv_frame(port, args.verbose)
            if jpeg is None:
                continue

            # Decode JPEG → BGR array
            arr = np.frombuffer(jpeg, dtype=np.uint8)
            img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if img is None:
                if args.verbose:
                    print(f"[WARN] cv2.imdecode failed on {len(jpeg)}-byte payload")
                continue

            total_frames += 1
            fps_frames   += 1

            # Update FPS counter once per second
            now = time.perf_counter()
            if now - t_fps >= 1.0:
                fps      = fps_frames / (now - t_fps)
                fps_frames = 0
                t_fps    = now

            if args.verbose:
                h, w = img.shape[:2]
                print(f"frame {total_frames:6d}  {len(jpeg):6d} B  "
                      f"{w}×{h}  {fps:.1f} fps")

            # Save to disk if requested
            if save_dir:
                out_path = save_dir / f"frame_{save_idx:06d}.jpg"
                out_path.write_bytes(jpeg)
                save_idx += 1

            # Overlay FPS
            cv2.putText(img, f"{fps:.1f} fps", (10, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 220, 0), 2, cv2.LINE_AA)

            cv2.imshow("ESP32-P4 Camera", img)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):   # q or Esc
                break

    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    except IOError as e:
        print(f"\nSerial error: {e}", file=sys.stderr)
    finally:
        port.close()
        cv2.destroyAllWindows()
        print(f"Total frames received: {total_frames}")


if __name__ == "__main__":
    main()

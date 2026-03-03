"""
ESP32-P4 Camera Viewer
Receives JPEG frames from ESP32-P4 cam_tester over USB serial and displays them.

Usage:
    python cam_viewer.py [COM_PORT] [BAUDRATE]
    python cam_viewer.py              # auto-detect port, 2000000 baud
    python cam_viewer.py COM3         # specific port, 2000000 baud
    python cam_viewer.py COM3 2000000 # explicit port and baud

Controls:
    Q or ESC - Quit
    S        - Save current frame as screenshot
"""

import sys
import time
import struct
import serial
import serial.tools.list_ports
import numpy as np
import cv2

# Frame protocol constants (must match cam_tester_main.c)
FRAME_HEADER = b'\xAA\x55\xAA\x55'
FRAME_FOOTER = b'\xFF\xD9'
MAX_JPEG_SIZE = 200 * 1024  # 200KB safety limit

DEFAULT_BAUD = 2000000


def find_esp32_port():
    """Auto-detect ESP32-P4 serial port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        desc = (port.description or "").lower()
        # CH343 is the USB-serial chip on ESP32-P4 Nano
        if "ch343" in desc or "ch340" in desc or "esp" in desc or "usb" in desc.lower():
            if port.device != "COM1":  # skip COM1 (usually system port)
                return port.device
    # Fallback: return the first non-COM1 port
    for port in ports:
        if port.device != "COM1":
            return port.device
    return None


def sync_to_header(ser):
    """Scan serial stream until we find FRAME_HEADER."""
    buf = b''
    bytes_read = 0
    start_time = time.time()
    
    while True:
        byte = ser.read(1)
        if not byte:
            if time.time() - start_time > 2.0:
                print(f"[DEBUG] Timeout waiting for header. Total bytes read so far: {bytes_read}")
                start_time = time.time()
            continue
            
        bytes_read += 1
        buf += byte
        
        if bytes_read <= 32:
            print(f"[DEBUG] Byte {bytes_read:04d}: {byte.hex().upper()}")
            
        if bytes_read % 1000 == 0:
            print(f"[DEBUG] Read {bytes_read} bytes without finding header. Last 4 bytes: {buf[-4:].hex().upper() if len(buf)>=4 else buf.hex().upper()}")

        if len(buf) > 4:
            buf = buf[-4:]
        if buf == FRAME_HEADER:
            print(f"[DEBUG] SUCCESS: found FRAME_HEADER after {bytes_read} bytes!")
            return True


def receive_frame(ser):
    """Receive a single JPEG frame from the serial stream."""
    # Sync to frame header
    if not sync_to_header(ser):
        return None

    # Read 4-byte length (little-endian uint32)
    len_bytes = ser.read(4)
    if len(len_bytes) < 4:
        print(f"[DEBUG] Failed to read length bytes. Got {len(len_bytes)} bytes instead of 4.")
        return None

    jpeg_size = struct.unpack('<I', len_bytes)[0]
    print(f"[DEBUG] Parsed JPEG size: {jpeg_size} bytes")

    if jpeg_size == 0 or jpeg_size > MAX_JPEG_SIZE:
        print(f"[DEBUG] Invalid JPEG size: {jpeg_size} (Max is {MAX_JPEG_SIZE}). Dropping frame.")
        return None

    # Read JPEG data
    print(f"[DEBUG] Reading {jpeg_size} bytes of JPEG data...")
    jpeg_data = b''
    remaining = jpeg_size
    while remaining > 0:
        chunk = ser.read(min(remaining, 4096))
        if not chunk:
            print(f"[DEBUG] Connection lost while reading JPEG data. Remaining: {remaining}")
            return None
        jpeg_data += chunk
        remaining -= len(chunk)

    # Read and verify footer
    footer = ser.read(2)
    if footer != FRAME_FOOTER:
        print(f"[DEBUG] Footer mismatch! Expected {FRAME_FOOTER.hex().upper()}, got {footer.hex().upper() if len(footer)>0 else 'EOF'}")
        # Footer mismatch — might be out of sync, skip this frame
        return None

    print(f"[DEBUG] Successfully received frame of {len(jpeg_data)} bytes!")
    return jpeg_data


def main():
    # Parse arguments
    port = None
    baud = DEFAULT_BAUD

    if len(sys.argv) >= 2:
        port = sys.argv[1]
    if len(sys.argv) >= 3:
        baud = int(sys.argv[2])

    # Auto-detect port
    if not port:
        port = find_esp32_port()
        if not port:
            print("ERROR: No ESP32 serial port found. Specify port as argument.")
            print("Available ports:")
            for p in serial.tools.list_ports.comports():
                print(f"  {p.device}: {p.description}")
            sys.exit(1)

    print(f"=== ESP32-P4 Camera Viewer ===")
    print(f"Port:     {port}")
    print(f"Baudrate: {baud}")
    print(f"Controls: Q/ESC=Quit, S=Screenshot")
    print()

    # Open serial connection
    try:
        ser = serial.Serial(port, baud, timeout=2)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {port}: {e}")
        sys.exit(1)

    print("Waiting for frames...")

    frame_count = 0
    fps_start = time.time()
    fps_display = 0.0

    cv2.namedWindow("ESP32-P4 Camera", cv2.WINDOW_NORMAL)

    try:
        while True:
            # Receive JPEG frame
            jpeg_data = receive_frame(ser)
            if jpeg_data is None:
                continue

            # Decode JPEG to image
            np_arr = np.frombuffer(jpeg_data, dtype=np.uint8)
            frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
            if frame is None:
                continue

            frame_count += 1

            # Calculate FPS
            elapsed = time.time() - fps_start
            if elapsed >= 1.0:
                fps_display = frame_count / elapsed
                frame_count = 0
                fps_start = time.time()

            # Draw FPS overlay
            cv2.putText(frame, f"FPS: {fps_display:.1f}", (10, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2)

            # Show frame
            cv2.imshow("ESP32-P4 Camera", frame)

            # Handle keyboard input
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q') or key == 27:  # Q or ESC
                break
            elif key == ord('s'):
                filename = f"cam_screenshot_{int(time.time())}.jpg"
                cv2.imwrite(filename, frame)
                print(f"Screenshot saved: {filename}")

    except KeyboardInterrupt:
        print("\nInterrupted by user")
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print("Viewer closed.")


if __name__ == "__main__":
    main()

# Desktop Turret — Claude Context

## Project overview
Embedded control system for a desktop turret:
- **Camera streaming** — ESP32-P4 Nano + OV5647 via MIPI-CSI ribbon cable
- **Servo control** — ESP32-C3 (360° continuous + position) via Arduino
- **Trigger** — solenoid/servo pull via ESP32-C3
- **PC side** — Python scripts for live video and servo/trigger testing

---

## Hardware

| Board | Chip | Role | Port |
|---|---|---|---|
| ESP32-P4 Nano | ESP32-P4 | Camera (MIPI-CSI) | COM3 |
| ESP32-C3 | ESP32-C3 | Servos + trigger | varies |

**Camera wiring (ESP32-P4 Nano CAM connector):**
- SCCB SDA → GPIO7
- SCCB SCL → GPIO8
- MIPI-CSI data lanes → internal (ribbon cable, no GPIO assignment needed)
- MIPI LDO channel 3 @ 2500 mV (required for MIPI PHY)

**Serial baudrate:** 2 Mbaud (both boot console and streaming — fixed in sdkconfig).

---

## Toolchains

### ESP-IDF (camera — `cam_tester/`)
- **SDK:** ESP-IDF v5.4.1 at `C:\Espressif\frameworks\esp-idf-v5.4.1`
- **Target:** `esp32p4`
- **Build/flash:**
  ```powershell
  .\build_cam_tester.ps1           # build only
  .\build_cam_tester.ps1 flash     # build + flash (COM3)
  .\build_cam_tester.ps1 monitor   # build + flash + serial monitor
  ```
  Or directly:
  ```bash
  cd cam_tester
  idf.py set-target esp32p4   # first time only
  idf.py -p COM3 build flash monitor
  ```
- **Key components** (`idf_component.yml`):
  - `espressif/esp_cam_sensor` — OV5647 driver
  - `espressif/esp_sccb_intf` — SCCB/I2C camera bus

### Arduino CLI (servos / trigger — `*.ino` sketches)
- **FQBN:** `esp32:esp32:esp32p4` (P4 sketches), `esp32:esp32:esp32c3` (C3 sketches)
- **Build/flash:**
  ```powershell
  .\compile_and_upload_p4.ps1 .\servo_manual_test\servo_manual_test.ino
  .\compile_and_upload_c3.ps1 .\trigger_pull_test\trigger_pull_test.ino
  ```

---

## Camera pipeline
```
OV5647 sensor
  └─ MIPI-CSI (2 lanes, 200 Mbps, RAW8 800×640 @ 50 FPS)
       └─ CSI controller
            └─ ISP (RAW8 → RGB565)
                 └─ JPEG HW encoder (quality 25 → ~10–18 KB/frame)
                      └─ UART0 @ 2 Mbaud → CH343 USB-serial → PC
                           └─ cam_viewer.py (OpenCV display)
```

### Wire protocol (UART → PC)
```
[0xAA 0x55 0xAA 0x55]  magic header (4 bytes)
[length]               4 bytes, little-endian uint32
[JPEG data]            `length` bytes
[0xFF 0xD9]            footer (2 bytes)
```
Both `cam_tester_main.c` and `cam_viewer.py` must agree on this format.

---

## Key design constraints

### Binary UART output — always use `uart_write_bytes()`
Never use `fwrite`/`printf`/`fprintf` for binary frame data. The VFS layer
inserts `0x0D` before every `0x0A` byte (CRLF translation), silently corrupting
JPEG frames. `uart_write_bytes()` is raw — no VFS, no translation.

### Baudrate — fixed in sdkconfig, not at runtime
`CONFIG_ESP_CONSOLE_UART_BAUDRATE=2000000` in `sdkconfig.defaults`.
No `uart_set_baudrate()` at runtime. A runtime switch leaves residual bytes
in the TX FIFO that get sent at the wrong rate.

### UART driver — must be installed before streaming
`uart_driver_install(UART_NUM, 0, 128*1024, 0, NULL, 0)` gives a 128 KB TX
ring buffer. Without it, `uart_write_bytes()` fails (driver not installed).
The console UART does NOT have a driver installed by default in ESP-IDF 5.x.

### Cache sync before JPEG encode
After `esp_cam_ctlr_receive()`, call:
```c
esp_cache_msync(buf, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
```
This invalidates D-cache so the CPU sees what DMA wrote to SPIRAM.

### Logging during streaming
Disable with `esp_log_level_set("*", ESP_LOG_NONE)` before the streaming loop.
Log output shares UART0 with the binary stream — any log byte breaks the protocol.
A 50 ms delay before disabling lets queued log bytes drain.

---

## Python environment
```bash
pip install -r requirements.txt   # pyserial, opencv-python, numpy
python cam_viewer.py              # auto-detect port, 2 Mbaud
python cam_viewer.py COM3 -v      # explicit port, verbose per-frame debug
```

---

## Sketches reference

| Sketch | Board | Script |
|---|---|---|
| `servo_manual_test/` | C3 | — (interactive serial) |
| `servo_position_control/` | C3 | `run_position_test.py` |
| `servo_velocity_test/` | C3 | `run_velocity_test.py` |
| `trigger_pull_test/` | C3 | `trigger_pull.py` |
| `magnetic_encoder_test/` | C3 | — |
| `cam_tester/` | **P4** | `cam_viewer.py` |

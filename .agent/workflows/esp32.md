---
description: Compile, upload, and monitor ESP32 projects using Arduino CLI
---

# ESP32-C3 Development Workflow (Arduino CLI)

## Board Info
- **Board**: ESP32-C3
- **FQBN**: `esp32:esp32:esp32c3`
- **Port**: COM5
- **Baudrate**: 115200
- **Required option**: `CDCOnBoot=cdc` (needed for serial monitor over native USB)

## Prerequisites
- Arduino CLI installed (`winget install ArduinoSA.CLI`)
- ESP32 board core installed (`arduino-cli core install esp32:esp32`)
- If `arduino-cli` isn't found, refresh PATH first:
  ```powershell
  $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
  ```

## Detect COM Port
// turbo
1. List connected boards to find the ESP32's COM port:
   ```
   arduino-cli board list
   ```

## Compile a Sketch
// turbo
2. Compile the sketch (from the project directory containing the .ino file):
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc ./
   ```

## Upload to ESP32
3. Upload the compiled sketch:
   ```
   arduino-cli upload --fqbn esp32:esp32:esp32c3 -p COM5 ./
   ```

## Compile + Upload in One Step
4. Compile and upload together (most common):
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -u -p COM5 ./
   ```

## Serial Monitor
5. Open the serial monitor:
   ```
   arduino-cli monitor -p COM5 -c baudrate=115200
   ```
   **Note:** The serial monitor holds the COM port. It must be stopped (terminated) before uploading.

## Install Libraries
// turbo
6. Search and install libraries as needed:
   ```
   arduino-cli lib search <keyword>
   arduino-cli lib install "<Library Name>"
   ```

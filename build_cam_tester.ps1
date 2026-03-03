# Build, flash, and monitor the cam_tester ESP-IDF project
# Usage:
#   .\build_cam_tester.ps1           # Build only
#   .\build_cam_tester.ps1 flash     # Build + flash
#   .\build_cam_tester.ps1 monitor   # Build + flash + monitor

param(
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action = "build"
)

$IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.4.1"
$PROJECT_DIR = "$PSScriptRoot\cam_tester"
$PORT = "COM3"

# Source ESP-IDF environment
Write-Host "Loading ESP-IDF environment..." -ForegroundColor Cyan
. "$IDF_PATH\export.ps1"

Write-Host ""
Write-Host "=== ESP32-P4 Camera Tester Build ===" -ForegroundColor Cyan
Write-Host "Project: $PROJECT_DIR"
Write-Host "Action:  $Action"
Write-Host ""

Push-Location $PROJECT_DIR

try {
    # Set target if not already set
    if (-not (Test-Path "build\CMakeCache.txt")) {
        Write-Host "Setting target to esp32p4..." -ForegroundColor Yellow
        idf.py set-target esp32p4
    }

    switch ($Action) {
        "build" {
            idf.py build
        }
        "flash" {
            idf.py -p $PORT build flash
        }
        "monitor" {
            idf.py -p $PORT build flash monitor
        }
    }

    if ($LASTEXITCODE -eq 0) {
        Write-Host "`nDone!" -ForegroundColor Green
    }
    else {
        Write-Host "`nFailed with exit code $LASTEXITCODE" -ForegroundColor Red
    }
}
finally {
    Pop-Location
}

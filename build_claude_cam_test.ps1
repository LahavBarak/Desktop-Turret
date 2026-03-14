# build_claude_cam_test.ps1 — build / flash / monitor claude_cam_test
#
# Usage (from repo root, with ESP-IDF environment active):
#   .\build_claude_cam_test.ps1                    # build only
#   .\build_claude_cam_test.ps1 flash              # build + flash COM3
#   .\build_claude_cam_test.ps1 monitor            # build + flash + serial monitor
#   .\build_claude_cam_test.ps1 flash   -Port COM5 # different port
#   .\build_claude_cam_test.ps1 monitor -Port COM5

param(
    [string]$Action = "build",
    [string]$Port   = "COM3"
)

$ErrorActionPreference = "Stop"

$IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.4.1"
Write-Host "Loading ESP-IDF environment..." -ForegroundColor Cyan
. "$IDF_PATH\export.ps1"

$ProjectDir = Join-Path $PSScriptRoot "claude_cam_test"

Push-Location $ProjectDir
try {
    # First-time target selection
    if (-not (Test-Path "build\CMakeCache.txt")) {
        if (Test-Path "build") {
            Write-Host "Cleaning invalid build directory..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force build
        }
        Write-Host "Setting target to esp32p4..." -ForegroundColor Yellow
        idf.py set-target esp32p4
    }

    switch ($Action.ToLower()) {
        "build"   { idf.py build }
        "flash"   { idf.py -p $Port build flash }
        "monitor" { idf.py -p $Port build flash monitor }
        default   { Write-Error "Unknown action '$Action'. Use: build | flash | monitor" }
    }
} finally {
    Pop-Location
}

# compile_and_upload.ps1 - Auto-detect ESP32 board and compile/upload a sketch
# Usage: .\compile_and_upload.ps1 <path\to\sketch.ino> [-Board c3|p4] (or simply without '-Board')

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$SketchPath,

    [Parameter(Mandatory=$false, Position=1)]
    [Alias("b")]
    [string]$Board = ""
)

# Board definitions: chip type -> FQBN
$boards = @{
    "ESP32-C3" = "esp32:esp32:XIAO_ESP32C3:CDCOnBoot=default"
    "ESP32-P4" = "esp32:esp32:esp32p4"
}

# VID:PID fallback for boards that aren't auto-identified
$vidpid = @{
    "0x303A:0x1001" = "ESP32-C3"   # Espressif ESP32-C3
    "0x1A86:0x55D3" = "ESP32-P4"   # CH343 USB-serial on P4 Nano
}

# Normalize optional board filter (c3 -> C3, p4 -> P4)
$boardFilter = $Board.ToUpper()

# Detect board
Write-Host "Scanning for ESP32 board..." -ForegroundColor Cyan
$json = arduino-cli board list --format json | ConvertFrom-Json
$portList = if ($json.detected_ports) { $json.detected_ports } else { $json }

$detected = $null
foreach ($entry in $portList) {
    $portInfo = $entry.port

    # Method 1: matching_boards (arduino-cli identified the board)
    if ($entry.matching_boards) {
        foreach ($board in $entry.matching_boards) {
            foreach ($chip in $boards.Keys) {
                if ($boardFilter -and $chip -notlike "*$boardFilter*") { continue }
                if ($board.fqbn -like "*$($chip.ToLower().Replace('-',''))*" -or $board.name -like "*$chip*") {
                    $detected = @{ Port = $portInfo.address; Chip = $chip; FQBN = $boards[$chip]; Name = $board.name }
                    break
                }
            }
            if ($detected) { break }
        }
    }

    # Method 2: VID:PID fallback
    if (-not $detected -and $portInfo.properties.vid -and $portInfo.properties.pid) {
        $key = "$($portInfo.properties.vid):$($portInfo.properties.pid)"
        if ($vidpid.ContainsKey($key)) {
            $chip = $vidpid[$key]
            if (-not $boardFilter -or $chip -like "*$boardFilter*") {
                $detected = @{ Port = $portInfo.address; Chip = $chip; FQBN = $boards[$chip]; Name = "$chip (detected via USB VID:PID)" }
            }
        }
    }

    if ($detected) { break }
}

if (-not $detected) {
    if ($boardFilter) {
        Write-Host "ESP32-$boardFilter not found. Make sure it's plugged in." -ForegroundColor Red
    } else {
        Write-Host "No supported ESP32 board found. Make sure it's plugged in." -ForegroundColor Red
        Write-Host "Supported boards: $($boards.Keys -join ', ')"
    }
    exit 1
}

$resolvedPath = Resolve-Path $SketchPath -ErrorAction Stop

Write-Host ""
Write-Host "=== Compiling & Uploading ===" -ForegroundColor Cyan
Write-Host "Board:  $($detected.Name) ($($detected.Chip))"
Write-Host "FQBN:   $($detected.FQBN)"
Write-Host "Port:   $($detected.Port)"
Write-Host "Sketch: $resolvedPath"
Write-Host ""

# Run compile+upload in foreground for maximum speed
Write-Host "Compiling & Uploading... " -ForegroundColor Yellow
arduino-cli compile --fqbn $($detected.FQBN) -u -p $($detected.Port) $resolvedPath.ToString()

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nDone! Upload successful." -ForegroundColor Green
} else {
    Write-Host "`nFailed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

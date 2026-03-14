param(
    [int]$Baudrate = 115200
)

# Detect board port
Write-Host "Scanning for ESP32 board..." -ForegroundColor Cyan
$json = arduino-cli board list --format json | ConvertFrom-Json
$portList = if ($json.detected_ports) { $json.detected_ports } else { $json }

$port = $null
$name = "Unknown"
foreach ($entry in $portList) {
    $portInfo = $entry.port
    # Skip non-USB ports (e.g. COM1)
    if ($portInfo.protocol_label -notlike "*USB*") { continue }

    if ($entry.matching_boards) {
        $port = $portInfo.address
        $name = $entry.matching_boards[0].name
    } else {
        $port = $portInfo.address
        $name = "ESP32 (USB device)"
    }
    break
}

if (-not $port) {
    Write-Host "No ESP32 board found. Make sure it's plugged in." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== Serial Monitor ===" -ForegroundColor Cyan
Write-Host "Board:    $name"
Write-Host "Port:     $port"
Write-Host "Baudrate: $Baudrate"
Write-Host "Press CTRL-C to exit."
Write-Host ""

try {
    while ($true) {
        $ports = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($ports -contains $port) {
            arduino-cli monitor -p $port -c baudrate=$Baudrate
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Connection lost. Waiting for device to reconnect..." -ForegroundColor Yellow
                Start-Sleep -Seconds 1
            }
        } else {
            Start-Sleep -Milliseconds 500
        }
    }
} catch {
    # Handles CTRL-C termination
}

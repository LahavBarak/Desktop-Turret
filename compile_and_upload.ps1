param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$SketchPath
)

# Board definitions: chip type -> FQBN
$boards = @{
    "ESP32-C3" = "esp32:esp32:XIAO_ESP32C3:CDCOnBoot=cdc"
    "ESP32-P4" = "esp32:esp32:esp32p4"
}

# VID:PID fallback for boards that aren't auto-identified
$vidpid = @{
    "0x303A:0x1001" = "ESP32-C3"   # Espressif ESP32-C3
    "0x1A86:0x55D3" = "ESP32-P4"   # CH343 USB-serial on P4 Nano
}

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
            $detected = @{ Port = $portInfo.address; Chip = $chip; FQBN = $boards[$chip]; Name = "$chip (detected via USB VID:PID)" }
        }
    }

    if ($detected) { break }
}

if (-not $detected) {
    Write-Host "No supported ESP32 board found. Make sure it's plugged in." -ForegroundColor Red
    Write-Host "Supported boards: $($boards.Keys -join ', ')"
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

# Run compile+upload as a background job with a spinner
$job = Start-Job -ScriptBlock {
    param($fqbn, $port, $sketch)
    arduino-cli compile --fqbn $fqbn -u -p $port $sketch 2>&1
} -ArgumentList $detected.FQBN, $detected.Port, $resolvedPath.ToString()

$spinner = @('|', '/', '-', '\')
$i = 0
$phase = "Compiling"
Write-Host -NoNewline "$phase... "

while ($job.State -eq 'Running') {
    Write-Host -NoNewline "`r$phase... $($spinner[$i % 4]) " -ForegroundColor Yellow
    Start-Sleep -Milliseconds 200
    $i++

    # Check for partial output to detect phase change
    $partial = Receive-Job $job -ErrorAction SilentlyContinue
    if ($partial) {
        foreach ($line in $partial) {
            $text = $line.ToString()
            if ($text -match "Uploading|Serial port|Connecting") {
                if ($phase -eq "Compiling") {
                    Write-Host "`r$phase... done!        " -ForegroundColor Green
                    $phase = "Uploading"
                }
            }
            # Stream upload/result lines
            if ($phase -eq "Uploading" -or $text -match "error|Error|fatal") {
                Write-Host $text
            }
        }
    }
}

# Collect any remaining output
Write-Host "`r$phase... done!        " -ForegroundColor Green
$remaining = Receive-Job $job -ErrorAction SilentlyContinue
if ($remaining) {
    foreach ($line in $remaining) {
        Write-Host $line.ToString()
    }
}

$result = $job.ChildJobs[0].JobStateInfo
Remove-Job $job -Force

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nDone! Upload successful." -ForegroundColor Green
} else {
    Write-Host "`nFailed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

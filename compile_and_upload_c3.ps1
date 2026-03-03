param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$SketchPath
)

$FQBN = "esp32:esp32:esp32c3:CDCOnBoot=cdc"
$PORT = "COM5"

$resolvedPath = Resolve-Path $SketchPath -ErrorAction Stop

Write-Host "=== Compiling & Uploading to ESP32-C3 ===" -ForegroundColor Cyan
Write-Host "Sketch: $resolvedPath"
Write-Host "Port:   $PORT"
Write-Host ""

arduino-cli compile --fqbn $FQBN -u -p $PORT $resolvedPath

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nDone! Upload successful." -ForegroundColor Green
} else {
    Write-Host "`nFailed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

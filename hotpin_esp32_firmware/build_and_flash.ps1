# ESP32 Firmware Build and Flash Script
# Run this after the WebSocket simultaneous streaming fix

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "ESP32 Firmware Build & Flash Utility" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Change to firmware directory
Set-Location "F:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware"

# Load ESP-IDF environment
Write-Host "[1/3] Loading ESP-IDF environment..." -ForegroundColor Yellow
& "C:\Espressif\frameworks\esp-idf-v5.4.2\export.ps1"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to load ESP-IDF environment!" -ForegroundColor Red
    Write-Host "Make sure ESP-IDF is installed at: C:\Espressif\frameworks\esp-idf-v5.4.2" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[2/3] Building firmware..." -ForegroundColor Yellow
idf.py build

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build successful!" -ForegroundColor Green
Write-Host ""

# Ask user if they want to flash
$response = Read-Host "Do you want to flash the firmware to COM7? (y/n)"

if ($response -eq "y" -or $response -eq "Y") {
    Write-Host ""
    Write-Host "[3/3] Flashing firmware to COM7..." -ForegroundColor Yellow
    idf.py -p COM7 flash monitor
} else {
    Write-Host ""
    Write-Host "Skipping flash. You can flash manually with:" -ForegroundColor Cyan
    Write-Host "  idf.py -p COM7 flash monitor" -ForegroundColor White
}

Write-Host ""
Write-Host "Done!" -ForegroundColor Green

@echo off
setlocal enabledelayedexpansion

:: Configuration
set "ESP_IP=Fullsword.local"
set "ESP_PORT=3232"
set "AUTH_PASS=sword"

:: Try to find espota.exe
set "OTA_TOOL=C:\Users\Admin\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.3\tools\espota.exe"
if not exist "%OTA_TOOL%" (
    set "OTA_TOOL=C:\Users\Admin\AppData\Local\Arduino15\packages\arduino\hardware\esp32\2.0.18-arduino.5\tools\espota.exe"
)

echo [OTA UPLOAD] Target: %ESP_IP%:%ESP_PORT%

:: Check if binary exists locally (Exported build)
:: Both build variants might exist
set "BIN_FILE=Fullsword.ino.bin"

if not exist "%BIN_FILE%" (
    echo [ERROR] %BIN_FILE% not found in current directory.
    echo [TIP] In Arduino IDE, go to: Sketch -> Export Compiled Binary (Ctrl+Alt+S)
    echo Then run this script again.
    pause
    exit /b 1
)

:: Run upload
echo [OTA UPLOAD] Uploading %BIN_FILE%...
"%OTA_TOOL%" -i %ESP_IP% -p %ESP_PORT% -a %AUTH_PASS% -f "%BIN_FILE%"

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Upload complete!
    timeout /t 5
) else (
    echo [ERROR] Upload failed with code %ERRORLEVEL%.
    echo Check if the Fullsword is powered on and connected to WiFi.
    pause
)

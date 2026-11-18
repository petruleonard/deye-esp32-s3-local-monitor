@echo off
setlocal
chcp 65001 >nul

REM ============================================================================
REM Modifica portul COM de mai jos dupa ce conectezi placuta ESP32-S3 la PC:
set COM_PORT=COM7
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "PYTHON_EXE=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"

echo ============================================================
echo  Incarcare firmware Deye ESP32-S3 Bridge pe %COM_PORT%
echo ============================================================
echo.

if not exist "%PYTHON_EXE%" (
    echo [EROARE] Nu am gasit Python ESP-IDF la:
    echo   %PYTHON_EXE%
    echo.
    pause
    exit /b 1
)

if not exist "build\deye_esp32_s3_bridge.bin" (
    echo [EROARE] Nu am gasit build\deye_esp32_s3_bridge.bin!
    echo Ruleaza mai intai compilarea firmware-ului ^(idf.py build^).
    echo.
    pause
    exit /b 1
)

echo Scriu bootloader, partition table si firmware pe %COM_PORT%...
echo.

"%PYTHON_EXE%" -m esptool --chip esp32s3 -p %COM_PORT% -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 "build\bootloader\bootloader.bin" 0x8000 "build\partition_table\partition-table.bin" 0xf000 "build\ota_data_initial.bin" 0x20000 "build\deye_esp32_s3_bridge.bin"

if errorlevel 1 (
    echo.
    echo [EROARE] Scrierea pe %COM_PORT% a esuat!
    echo Verifica:
    echo  1. Portul COM din Device Manager ^(modifica set COM_PORT=... in acest fisier^)
    echo  2. Sa nu fie deschis alt program pe %COM_PORT% ^(ex. Serial Monitor^)
    echo  3. Daca nu intra automat in boot, tine apasat BOOT, apasa scurt RST, apoi elibereaza BOOT.
    echo.
    pause
    exit /b 1
)

echo.
echo [SUCCES] Firmware incarcat cu succes pe %COM_PORT%!
echo Placuta a repornit automat.
echo.
pause


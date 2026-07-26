@echo off
set MSYSTEM=
set MSYS2_PATH_TYPE=
set SHELL=
set CHERE_INVOKING=
set "IDF_PYTHON_ENV_PATH=E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;C:\Windows\system32;C:\Windows"

cd /d E:\esp_practice\first_practice

echo === FLASH ONLY ===
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" -m esptool --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 40m 0x2000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\display.bin 0xa10000 build\srmodels\srmodels.bin

if %ERRORLEVEL% equ 0 (
    echo FLASH SUCCESS
) else (
    echo FLASH FAILED
)
pause

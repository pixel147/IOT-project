@echo off
cd /d E:\esp_practice\first_practice
call E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4\export.bat >nul 2>&1
python -m esptool --chip esp32p4 -p COM7 -b 460800 --force write_flash 0x10000 build\display.bin
echo.
echo ===== FLASH DONE, STARTING MONITOR =====
echo.
idf.py monitor -p COM7

@echo off
SET PATH=C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem
SET IDF_PATH=E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4

cd /d E:\esp_practice\first_practice

REM Build
E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    pause
    exit /b %ERRORLEVEL%
)
echo BUILD SUCCESS

REM Flash
E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py -p COM7 flash
if %ERRORLEVEL% neq 0 (
    echo FLASH FAILED
    pause
    exit /b %ERRORLEVEL%
)
echo FLASH SUCCESS

REM Monitor
E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py monitor -p COM7
pause

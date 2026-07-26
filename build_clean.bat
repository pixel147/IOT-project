@echo off
REM Strip MSYS/MINGW from environment
set MSYSTEM=
set MSYS2_PATH_TYPE=
set SHELL=
set CHERE_INVOKING=

REM ESP-IDF paths
set "IDF_PATH=E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=E:\esp_idf\Espressif"
set "IDF_PYTHON_ENV_PATH=E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env"

REM Build path with ALL ESP-IDF toolchains but NO MSYS/MINGW
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\cmake\3.30.2\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\ninja\1.12.1"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\idf-exe\1.0.3"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"
set "PATH=%PATH%;%IDF_PATH%\tools"
set "PATH=%PATH%;C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem"

cd /d E:\esp_practice\first_practice

echo === CLEAN BUILD ===

REM Clean and build
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    pause
    exit /b %ERRORLEVEL%
)
echo BUILD SUCCESS

REM Flash
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -p COM7 flash
if %ERRORLEVEL% neq 0 (
    echo FLASH FAILED
    pause
    exit /b %ERRORLEVEL%
)
echo FLASH SUCCESS

REM Monitor - use raw python to avoid MSys
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" monitor -p COM7
pause

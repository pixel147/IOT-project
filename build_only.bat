@echo off
set MSYSTEM=
set MSYS2_PATH_TYPE=
set SHELL=
set CHERE_INVOKING=
set "IDF_PATH=E:\esp_idf\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=E:\esp_idf\Espressif"
set "IDF_PYTHON_ENV_PATH=E:\esp_idf\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\esp-clang\esp-19.1.2_20250312\esp-clang\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\cmake\3.30.2\bin"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\ninja\1.12.1"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\idf-exe\1.0.3"
set "PATH=%PATH%;%IDF_TOOLS_PATH%\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"
set "PATH=%PATH%;%IDF_PATH%\tools"
set "PATH=%PATH%;C:\Windows\system32;C:\Windows
cd /d E:\esp_practice\first_practice
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build > E:\esp_practice\first_practice\build_log.txt 2>&1
echo EXIT CODE: %ERRORLEVEL%

@echo off
chcp 65001 >nul
echo ====================================================
echo  G'lam OS Kernel - ESP32-S3 烧录与串口监视
echo ====================================================

set "PORT=COM7"
if not "%~1"=="" set "PORT=%~1"

set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%"

call C:\Espressif\frameworks\esp-idf-v5.5.6\export.bat
cd /d "%~dp0"
idf.py -p %PORT% flash monitor

pause

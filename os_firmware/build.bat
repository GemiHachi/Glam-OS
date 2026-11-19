@echo off
chcp 65001 >nul
echo ====================================================
echo  G'lam OS Kernel - ESP32-S3 一键编译脚本
echo ====================================================

set "IDF_COMPONENT_REGISTRY_URL=https://components.espressif.cn/"
set "PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%"

set "FW_DIR=%~dp0"
set "GLAMOR_BUILD=%FW_DIR%..\..\Glamor\app_sdk_template\build_apps.bat"

if exist "%GLAMOR_BUILD%" (
    echo [1/3] 先通过 Glamor SDK 编译打包动态 .elf 应用安装包 (.gla)...
    call "%GLAMOR_BUILD%"
    if errorlevel 1 exit /b 1
) else (
    echo [1/3] 未检测到同级 Glamor 目录，跳过示例 .gla 构建...
)

echo [2/3] 激活 ESP-IDF v5.5.6 环境...
call C:\Espressif\frameworks\esp-idf-v5.5.6\export.bat

echo [3/3] 编译 G'lam OS 内核 (目标 esp32s3)...
cd /d "%FW_DIR%"
idf.py set-target esp32s3
idf.py build

if %ERRORLEVEL% equ 0 (
    echo ====================================================
    echo  G'lam OS 编译成功！运行 flash.bat 进行烧录
    echo ====================================================
) else (
    echo ====================================================
    echo  编译失败，请检查上方日志
    echo ====================================================
)
pause

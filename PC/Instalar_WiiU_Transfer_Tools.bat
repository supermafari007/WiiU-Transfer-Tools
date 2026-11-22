@echo off
title Instalador - Wii U Transfer Tools v2.0.2
cd /d "%~dp0"
python -c "import webview, PIL" >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Instalando dependencias necesarias (pywebview y Pillow)...
    python -m pip install -r "%~dp0requirements.txt"
)
where pythonw >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    start "" pythonw "%~dp0Instalar_WiiU_Transfer_Tools.py"
) else (
    python "%~dp0Instalar_WiiU_Transfer_Tools.py"
)

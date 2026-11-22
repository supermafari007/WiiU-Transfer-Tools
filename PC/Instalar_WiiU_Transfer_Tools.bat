@echo off
title Instalador - Wii U Transfer Tools v2.0.0
cd /d "%~dp0"
where pythonw >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    start "" pythonw "%~dp0Instalar_WiiU_Transfer_Tools.py"
) else (
    python "%~dp0Instalar_WiiU_Transfer_Tools.py"
)

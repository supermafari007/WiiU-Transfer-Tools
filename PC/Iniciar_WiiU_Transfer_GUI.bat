@echo off
title Wii U Transfer Tools v2.0.0
cd /d "%~dp0"
where pythonw >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    start "" pythonw "%~dp0wiiu_transfer_gui.py"
) else (
    python "%~dp0wiiu_transfer_gui.py"
)

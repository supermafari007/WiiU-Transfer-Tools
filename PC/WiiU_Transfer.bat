@echo off
title Wii U Transfer
cd /d "%~dp0"
if not exist packages mkdir packages
where pythonw >nul 2>&1
if %ERRORLEVEL%==0 (
    start "" pythonw wiiu_transfer_gui.py
) else (
    python wiiu_transfer_gui.py
)

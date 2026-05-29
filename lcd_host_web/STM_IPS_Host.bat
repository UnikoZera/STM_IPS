@echo off
title STM IPS Host
cd /d "%~dp0"

:: Auto-install dependencies
python -q -m pip install -r requirements.txt 2>nul

:: Launch (keep console visible for server logs)
python launcher.py
pause

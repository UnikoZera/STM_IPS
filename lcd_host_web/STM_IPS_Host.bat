@echo off
setlocal
title STM IPS Host
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [!!] 未找到 python，请先安装 Python 3.10+ 并加入 PATH
  pause
  exit /b 1
)

echo [..] 检查依赖 flask / imageio-ffmpeg ...
python -m pip install -r requirements.txt -q
if errorlevel 1 (
  echo [!!] pip 安装依赖失败
  pause
  exit /b 1
)

echo [..] 启动 launcher.py ...
python launcher.py
set ERR=%ERRORLEVEL%
echo.
if not "%ERR%"=="0" echo [!!] 进程退出码 %ERR%
pause
exit /b %ERR%

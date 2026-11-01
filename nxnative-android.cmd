@echo off
setlocal
cd /d "%~dp0"
set "PYTHONUTF8=1"
if exist "%~dp0.venv\Scripts\python.exe" (
    "%~dp0.venv\Scripts\python.exe" -m nxnative_android %*
) else (
    python -m nxnative_android %*
)
if errorlevel 1 pause

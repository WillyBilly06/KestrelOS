@echo off
rem Install-KestrelOS.cmd - double-click entry point.
rem
rem Re-launches itself elevated if it is not already, then hands over to the
rem PowerShell script that does the work.

setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-KestrelOS.ps1"

echo.
pause

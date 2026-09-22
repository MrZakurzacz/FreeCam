@echo off
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo FreeCam virtual camera installation requires Administrator rights.
    echo Right-click this file and choose "Run as administrator".
    pause
    exit /b 1
)

echo Registering FreeCam Camera...
%SystemRoot%\System32\regsvr32.exe /s "%~dp0FreeCamVirtualCamera.dll"

if %errorlevel% neq 0 (
    echo Registration failed with error %errorlevel%.
    pause
    exit /b %errorlevel%
)

echo.
echo FreeCam Camera registered successfully.
echo Restart any browser, Discord, Teams, OBS, or other app that was already open.
pause

@echo off
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo FreeCam virtual camera removal requires Administrator rights.
    echo Right-click this file and choose "Run as administrator".
    pause
    exit /b 1
)

echo Unregistering FreeCam Camera...
%SystemRoot%\System32\regsvr32.exe /s /u "%~dp0FreeCamVirtualCamera.dll"

if %errorlevel% neq 0 (
    echo Unregistration failed with error %errorlevel%.
    pause
    exit /b %errorlevel%
)

echo.
echo FreeCam Camera removed successfully.
pause

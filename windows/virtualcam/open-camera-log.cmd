@echo off
if not exist "%TEMP%\FreeCamVirtualCamera.log" (
    echo No FreeCam virtual camera trace exists yet.
    echo Start a camera app and try to use FreeCam Camera first.
    pause
    exit /b 1
)
notepad "%TEMP%\FreeCamVirtualCamera.log"

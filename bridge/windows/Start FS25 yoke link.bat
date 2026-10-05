@echo off
rem Starts the FS25 link and opens its settings page. Close this window or press Ctrl+C to release the yoke.
cd /d "%~dp0"
start "" /b cmd /c "timeout /t 4 /nobreak >nul & start http://127.0.0.1:8777/"
moza_fs_bridge.exe %*
echo.
echo The bridge has stopped.
pause

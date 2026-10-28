@echo off
setlocal
rem Double-click to clean. Add -Preview to inspect without deleting files.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\clean_keil.ps1" %*
set "clean_keil_result=%ERRORLEVEL%"
echo.
pause
exit /b %clean_keil_result%

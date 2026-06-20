@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_flexcurve.ps1" %*
exit /b %ERRORLEVEL%

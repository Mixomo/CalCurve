@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_calcurve.ps1" %*
exit /b %ERRORLEVEL%

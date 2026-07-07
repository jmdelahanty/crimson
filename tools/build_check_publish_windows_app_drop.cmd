@echo off
setlocal

powershell -ExecutionPolicy Bypass -File "%~dp0build_check_publish_windows_app_drop.ps1" %*
exit /b %ERRORLEVEL%

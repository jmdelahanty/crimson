@echo off
setlocal

powershell -ExecutionPolicy Bypass -File "%~dp0setup_windows_vcpkg.ps1" %*
exit /b %ERRORLEVEL%

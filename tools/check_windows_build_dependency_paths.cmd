@echo off
setlocal

powershell -ExecutionPolicy Bypass -File "%~dp0check_windows_build_dependency_paths.ps1" %*
exit /b %ERRORLEVEL%

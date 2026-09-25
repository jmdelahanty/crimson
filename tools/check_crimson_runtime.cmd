@echo off
setlocal
powershell -ExecutionPolicy Bypass -NoExit -File "%~dp0check_crimson_runtime.ps1" %*

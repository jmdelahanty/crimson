@echo off
setlocal
powershell -ExecutionPolicy Bypass -NoExit -File "%~dp0set_crimson_cuda_device.ps1" %*

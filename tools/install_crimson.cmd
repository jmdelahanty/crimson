@echo off
setlocal
powershell -ExecutionPolicy Bypass -NoExit -File "%~dp0install_crimson.ps1" %*

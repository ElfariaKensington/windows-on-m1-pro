@echo off
rem Installs the Apple M1 Pro GPU driver. Right-click > Run as administrator.
set "G=%~dp0gpu.ps1"
if not exist "%G%" set "G=%~dp0AppleGPU\gpu.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%G%" install
pause

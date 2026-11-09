@echo off
rem Removes the Apple M1 Pro GPU driver (Microsoft Basic Display returns). Run as administrator.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gpu.ps1" remove
pause

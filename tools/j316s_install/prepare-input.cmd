@echo off
setlocal EnableExtensions
set "log=%SystemRoot%\Panther\J316-input-install.log"
"%~dp0J316Tools.exe" /trust >> "%log%" 2>&1
if errorlevel 1 exit /b 1
bcdedit /set {current} testsigning on >> "%log%" 2>&1
if errorlevel 1 exit /b 1
pnputil /add-driver "%~dp0J316SpiInput.inf" /install >> "%log%" 2>&1
exit /b 0

@echo off
setlocal EnableExtensions
set "log=%SystemRoot%\Panther\J316-input-install.log"
powershell.exe -NoProfile -Command "$ErrorActionPreference='Stop'; $c=[System.Security.Cryptography.X509Certificates.X509Certificate2]::new('%~dp0J316SpiInput.cer'); if (((Get-Date) -lt $c.NotBefore) -or ((Get-Date) -gt $c.NotAfter)) { Set-Date ($c.NotBefore.AddMinutes(5)) }" >> "%log%" 2>&1
if errorlevel 1 exit /b 1
"%~dp0J316Tools.exe" /trust >> "%log%" 2>&1
if errorlevel 1 exit /b 1
bcdedit /set {current} testsigning on >> "%log%" 2>&1
if errorlevel 1 exit /b 1
pnputil /add-driver "%~dp0J316SpiInput.inf" /install >> "%log%" 2>&1
exit /b %ERRORLEVEL%

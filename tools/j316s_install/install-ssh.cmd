@echo off
rem Run inside the installed Windows (double-click or from a terminal).
rem Imports the GitHub SSH identity staged next to this file into %USERPROFILE%\.ssh,
rem restricts it to the current user, then removes the staged private key.
setlocal
set "SRC=%~dp0"
set "DST=%USERPROFILE%\.ssh"
if not exist "%DST%" md "%DST%"
copy /y "%SRC%id_ed25519" "%DST%\id_ed25519" >nul || goto fail
copy /y "%SRC%id_ed25519.pub" "%DST%\id_ed25519.pub" >nul || goto fail
if not exist "%DST%\config" copy /y "%SRC%config" "%DST%\config" >nul
type "%SRC%known_hosts" >> "%DST%\known_hosts"
icacls "%DST%\id_ed25519" /inheritance:r /grant:r "%USERNAME%:F" >nul || goto fail
del /f /q "%SRC%id_ed25519" 2>nul
echo SSH identity installed in %DST%. Testing GitHub...
ssh -o BatchMode=yes -T git@github.com
echo (A "successfully authenticated" message above means it works. No network yet is fine.)
pause
exit /b 0
:fail
echo FAILED to install the SSH identity.
pause
exit /b 1

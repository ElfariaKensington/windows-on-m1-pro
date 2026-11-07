@echo off
setlocal EnableExtensions
rem Restore the exact original values read from the pre-repair SYSTEM snapshot.
rem Never read the overwritten saved-return file or call this script recursively.
reg add HKLM\SYSTEM\Setup /v CmdLine /t REG_SZ /d "" /f
reg add HKLM\SYSTEM\Setup /v SetupType /t REG_DWORD /d 0 /f
reg add HKLM\SYSTEM\Setup /v SystemSetupInProgress /t REG_DWORD /d 0 /f
call C:\J316Input\prepare-input.cmd
if errorlevel 1 goto failed
pnputil /scan-devices
shutdown /r /t 0
exit /b
:failed
echo Driver installation failed. Log: %SystemRoot%\Panther\J316-input-install.log
cmd.exe

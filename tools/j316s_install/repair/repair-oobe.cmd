@echo off
setlocal EnableExtensions
set "original="
set /p original=<C:\J316Input\original-cmdline.txt
call C:\J316Input\prepare-input.cmd
if errorlevel 1 goto failed
reg add HKLM\SYSTEM\Setup /v CmdLine /t REG_SZ /d "%original%" /f
for %%V in (SetupType SystemSetupInProgress) do (
 set /p saved=<C:\J316Input\original-%%V.txt
 call reg add HKLM\SYSTEM\Setup /v %%V /t REG_DWORD /d %%saved%% /f
)
pnputil /scan-devices
if not defined original shutdown /r /t 0
if not defined original exit /b
pushd %SystemRoot%\System32
%original%
exit /b
:failed
echo Driver installation failed. Log: %SystemRoot%\Panther\J316-input-install.log
cmd.exe

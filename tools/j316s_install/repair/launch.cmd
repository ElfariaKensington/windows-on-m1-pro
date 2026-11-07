@echo off
wpeinit
mountvol /E
(echo san policy=OnlineAll
 echo automount enable
 echo rescan
 echo select disk 0
 echo online disk
 echo select partition 1
 echo assign letter=S
 echo select partition 3
 echo assign letter=W
 echo select partition 4
 echo assign letter=T
 echo select partition 5
 echo assign letter=U
 echo exit
)>X:\repair-disks.txt
diskpart /s X:\repair-disks.txt > X:\repair-disks.log 2>&1
set "W="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%D:\Windows\System32\Config\SYSTEM set "W=%%D:"
if not defined W goto failed
if not exist %W%\Windows\System32\Config\SYSTEM.J316-before-input-repair copy /y %W%\Windows\System32\Config\SYSTEM %W%\Windows\System32\Config\SYSTEM.J316-before-input-repair
if errorlevel 1 goto failed
reg load HKLM\J316Offline %W%\Windows\System32\Config\SYSTEM
if errorlevel 1 goto failed
set "original="
for /f "tokens=2,*" %%A in ('reg query HKLM\J316Offline\Setup /v CmdLine') do if "%%A"=="REG_SZ" set "original=%%B"
echo(%original%> %W%\J316Input\original-cmdline.txt
for %%V in (SetupType SystemSetupInProgress) do echo 0> %W%\J316Input\original-%%V.txt
for %%V in (SetupType SystemSetupInProgress) do for /f "tokens=2,*" %%A in ('reg query HKLM\J316Offline\Setup /v %%V') do if "%%A"=="REG_DWORD" echo %%B> %W%\J316Input\original-%%V.txt
copy /y X:\J316Install\repair-oobe.cmd %W%\J316Input\repair-oobe.cmd
copy /y X:\J316Install\prepare-input.cmd %W%\J316Input\prepare-input.cmd
reg add HKLM\J316Offline\Setup /v CmdLine /t REG_SZ /d "cmd.exe /c C:\J316Input\repair-oobe.cmd" /f
reg add HKLM\J316Offline\Setup /v SetupType /t REG_DWORD /d 2 /f
reg add HKLM\J316Offline\Setup /v SystemSetupInProgress /t REG_DWORD /d 1 /f
reg unload HKLM\J316Offline
if exist S:\EFI\Microsoft\Boot\bootmgfw.j316-backup ren S:\EFI\Microsoft\Boot\bootmgfw.j316-backup bootmgfw.efi
echo Repair prepared. Restarting into existing Windows.
wpeutil reboot
exit /b
:failed
echo Repair stopped safely. Keep this window open.
cmd.exe

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
rem Restore direct boot first, so an interrupted repair cannot loop into PE.
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%D:\EFI\Microsoft\Boot\bootmgfw.j316-backup ren %%D:\EFI\Microsoft\Boot\bootmgfw.j316-backup bootmgfw.efi
set "W="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%D:\Windows\System32\Config\SYSTEM set "W=%%D:"
if not defined W goto failed
if exist %W%\Windows\System32\Config\SYSTEM.J316-before-input-repair goto backedup
copy /y %W%\Windows\System32\Config\SYSTEM %W%\Windows\System32\Config\SYSTEM.J316-before-input-repair
if errorlevel 1 goto failed
:backedup
rem Service the offline Driver Store: input cannot approve an online publisher UI.
md %W%\J316Input\scratch 2>nul
dism /Image:%W%\ /Add-Driver /Driver:X:\J316Input\J316SpiInput.inf /ForceUnsigned /ScratchDir:%W%\J316Input\scratch /LogPath:%W%\Windows\Panther\J316-offline-input.log
if errorlevel 1 goto failed
dism /Image:%W%\ /Add-Driver /Driver:X:\J316Input\J316VhfInput.inf /ForceUnsigned /ScratchDir:%W%\J316Input\scratch /LogPath:%W%\Windows\Panther\J316-offline-input.log
if errorlevel 1 goto failed
reg load HKLM\J316Offline %W%\Windows\System32\Config\SYSTEM
if errorlevel 1 goto failed
reg add HKLM\J316Offline\Setup /v CmdLine /t REG_SZ /d "" /f
reg add HKLM\J316Offline\Setup /v SetupType /t REG_DWORD /d 0 /f
reg add HKLM\J316Offline\Setup /v SystemSetupInProgress /t REG_DWORD /d 0 /f
reg unload HKLM\J316Offline
echo Both offline input packages installed.>%W%\J316Input\offline-input-installed.txt
echo Input installed offline. Restarting into existing Windows.
wpeutil reboot
exit /b
:failed
echo Repair stopped. Direct Windows boot has been restored.
cmd.exe

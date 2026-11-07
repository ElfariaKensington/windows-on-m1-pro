@echo off
rem VM ONLY: disk 0 is the disposable virtual NVMe disk, never a host disk.
rem VhfProbe binds ROOT\J316VHFP and has no physical hardware resources.
wpeinit
(
    echo select disk 0
    echo select partition 1
    echo assign letter=S
    echo exit
) > X:\VhfProbe\assign.txt
diskpart /s X:\VhfProbe\assign.txt > X:\VhfProbe\assign.log 2>&1
X:\J316Input\J316Tools.exe /trust > S:\final-trust.log 2>&1
X:\VhfProbe\devcon.exe install X:\VhfProbe\VhfProbe.inf ROOT\J316VHFP > S:\final-probe-install.log 2>&1
start "J316 startup regression" /b cmd /c X:\J316Input\start.cmd > S:\final-startup.log 2>&1
X:\J316Input\J316Tools.exe /wait
X:\VhfProbe\InputObserver.exe > S:\final-observer.log 2>&1
echo %ERRORLEVEL% > S:\final-observer-result.txt
pnputil /enum-devices /class Keyboard /services /stack > S:\final-keyboard.log 2>&1
pnputil /enum-devices /class Mouse /services /stack > S:\final-mouse.log 2>&1
copy /y X:\J316Input\hid-bridge-*.log S:\
copy /y X:\Windows\INF\setupapi.dev.log S:\final-setupapi.log
wpeutil shutdown

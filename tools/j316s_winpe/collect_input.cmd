@echo off
setlocal EnableExtensions
:collect
(
    echo.
    echo ===== %DATE% %TIME% =====
    echo --- AppleInput device and assigned resources ---
    pnputil /enum-devices /deviceid "ACPI\APPL0001" /deviceids /drivers /services /stack /interfaces /resources
    echo --- AppleInput service ---
    sc query AppleInput
    echo --- AppleInput transport snapshot ---
    X:\J316sInput\AppleInputLiveDiag.exe
) >> X:\J316sInput\driver-runtime.log 2>&1
wevtutil qe System /c:100 /rd:true /f:text > X:\J316sInput\system-events-latest.log 2>&1
for %%F in (setupapi.dev.log setupapi.offline.log) do (
    if exist "%SystemRoot%\INF\%%F" copy /y "%SystemRoot%\INF\%%F" "X:\J316sInput\%%F" >nul 2>&1
)
for %%F in (setupact.log setuperr.log) do (
    if exist "%SystemRoot%\Panther\%%F" copy /y "%SystemRoot%\Panther\%%F" "X:\J316sInput\%%F" >nul 2>&1
)
call X:\J316sInput\persist_logs.cmd
timeout /t 10 /nobreak >nul
goto collect

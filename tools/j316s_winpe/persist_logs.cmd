@echo off
setlocal EnableExtensions
rem Retry discovery on each call. The NVMe volume may mount after wpeinit.
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%D:\J316S-DIAGNOSTICS.TARGET" (
        call :save "%%D:\J316sInputLogs"
        if not errorlevel 1 exit /b 0
    )
)
exit /b 1

:save
set "LOGROOT=%~1"
if not exist "%LOGROOT%" md "%LOGROOT%" 2>nul
copy /y "X:\J316sInput\boot.log" "%LOGROOT%\boot.log" >nul 2>&1
if errorlevel 1 exit /b 1
for %%F in (mountvol-enable.log trust.log trust-result.txt drvload.log drvload-result.txt pnp-scan.log diskpart.txt diskpart-rescan.txt disk-layout.log disk-layout-rescan.log pnp-all.log pnp-keyboard.log pnp-hid.log pnp-appleinput.log pnp-after-drvload.log pnp-appleinput-after-drvload.log service-after-drvload.log driver-store.log driver-store-after-drvload.log driver-services.log setupapi.dev.log setupapi.offline.log setupact.log setuperr.log system-events.log) do (
    if exist "X:\J316sInput\%%F" copy /y "X:\J316sInput\%%F" "%LOGROOT%\%%F" >nul 2>&1
)
if exist X:\J316sInput\driver-runtime.log copy /y X:\J316sInput\driver-runtime.log "%LOGROOT%\driver-runtime.log" >nul 2>&1
if exist X:\J316sInput\system-events-latest.log copy /y X:\J316sInput\system-events-latest.log "%LOGROOT%\system-events-latest.log" >nul 2>&1
exit /b 0

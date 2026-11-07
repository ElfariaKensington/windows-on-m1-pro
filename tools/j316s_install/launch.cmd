@echo off
setlocal EnableExtensions
set "source="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist %%D:\J316-INSTALL.SOURCE if exist %%D:\sources\install.swm if exist %%D:\sources\install2.swm set "source=%%D:"
)
if not defined source (
    echo Internal Windows installation source was not found.
    echo Do not format the diagnostics or macOS partitions.
    cmd.exe
    exit /b 1
)
echo Installing from internal source %source%.
echo Select the unallocated space on the 80 GB disk for Windows.
echo Keep J316SOURCE, System and Microsoft reserved partitions.
reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f >nul
reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f >nul
%source%\sources\setup.exe /installfrom:%source%\sources\install.swm /unattend:X:\J316Install\unattend.xml
exit /b %ERRORLEVEL%

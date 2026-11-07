@echo off
setlocal EnableExtensions
:again
X:\J316Input\J316Tools.exe > X:\J316Input\input.log 2>&1
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist %%D:\J316S-DIAGNOSTICS.TARGET (
        if not exist %%D:\J316FreshInput md %%D:\J316FreshInput
        copy /y X:\J316Input\*.log %%D:\J316FreshInput\ >nul 2>&1
        copy /y X:\Windows\INF\setupapi.dev.log %%D:\J316FreshInput\ >nul 2>&1
    )
)
timeout /t 3 /nobreak >nul 2>&1
goto again

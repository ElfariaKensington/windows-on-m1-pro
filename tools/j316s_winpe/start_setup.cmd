@echo off
wpeinit
X:\J316sInput\TrustAppleInput.exe > X:\J316sInput\trust.log 2>&1
set "TrustResult=%ERRORLEVEL%"
echo %TrustResult% > X:\J316sInput\trust-result.txt
drvload X:\J316sInput\AppleInput.inf > X:\J316sInput\drvload.log 2>&1
set "DrvloadResult=%ERRORLEVEL%"
echo %DrvloadResult% > X:\J316sInput\drvload-result.txt
(
    echo TrustAppleInput exit code: %TrustResult%
    echo drvload exit code: %DrvloadResult%
) > X:\J316sInput\input-status.txt
:setup
X:\sources\setup.exe

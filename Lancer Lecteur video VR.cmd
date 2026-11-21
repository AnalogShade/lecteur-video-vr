@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist "native\bin\LecteurVideoVR.exe" goto :build
if not exist "native\bin\libmpv-2.dll" goto :build
if not exist "native\bin\openxr_loader.dll" goto :build
goto :launch

:build
powershell.exe -NoProfile -File "native\build.ps1"
if errorlevel 1 goto :error

:launch
start "" "native\bin\LecteurVideoVR.exe" %1
popd
exit /b 0

:error
popd
pause
exit /b 1

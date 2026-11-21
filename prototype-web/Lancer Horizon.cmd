@echo off
setlocal
pushd "%~dp0" || exit /b 1
where node.exe >nul 2>nul
if errorlevel 1 (
  echo Node.js est requis pour lancer le prototype du lecteur video VR.
  goto :error
)
if not exist dist\app.js (
  call npm.cmd ci
  if errorlevel 1 goto :error
  call npm.cmd run build
  if errorlevel 1 goto :error
)
node.exe server.mjs --open
if errorlevel 1 goto :error
popd
exit /b 0

:error
popd
pause
exit /b 1

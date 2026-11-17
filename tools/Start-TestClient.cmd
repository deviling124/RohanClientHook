@echo off
rem Starts the TEST client copy (not your original NovaROHAN folder) with the lab server endpoint.
rem Double-click it. Windows will ask for administrator approval (UAC), like the normal launcher.
setlocal
set "CLIENT=E:\00_Adrian\ROHANPS\NovaROHAN-Launcher-Test\client"
set "ENDPOINT=https://172.24.27.44/"

if not exist "%CLIENT%\rohanclient_unpacked.exe" (
  echo Test client not found: %CLIENT%\rohanclient_unpacked.exe
  pause
  exit /b 1
)
tasklist /FI "IMAGENAME eq rohanclient_unpacked.exe" 2>nul | find /I "rohanclient_unpacked.exe" >nul
if not errorlevel 1 (
  echo A client is already running. Close it first so the new novaclient.dll is loaded.
  pause
  exit /b 1
)

echo Client folder : %CLIENT%
echo novaclient.ini: & type "%CLIENT%\novaclient.ini" 2>nul
echo.
echo Starting the client... approve the Windows prompt.
powershell -NoProfile -Command "Start-Process -FilePath '%CLIENT%\rohanclient_unpacked.exe' -ArgumentList '\"%ENDPOINT%\"' -WorkingDirectory '%CLIENT%' -Verb RunAs"
if errorlevel 1 (
  echo The client did not start ^(the Windows prompt may have been declined^).
  pause
  exit /b 1
)

rem Give the DLL a moment to write its startup line, then show it.
timeout /t 6 /nobreak >nul
echo.
echo --- last lines of novaclient.log (look for: client guard: mode=audit, hooks attached) ---
powershell -NoProfile -Command "Get-Content '%CLIENT%\novaclient.log' -Tail 8"
echo.
echo Log in and play. Tell Claude when you are in the world.
pause

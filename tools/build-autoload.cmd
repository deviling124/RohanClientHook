@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if "%~1"=="" (
  node tools/patch-client-autoload.mjs 00_reference/rohanclient_unpacked.exe bin/Autoload/rohanclient_unpacked.exe
) else (
  node tools/patch-client-autoload.mjs "%~1" "%~2"
)
set "buildResult=%errorlevel%"
popd
exit /b %buildResult%

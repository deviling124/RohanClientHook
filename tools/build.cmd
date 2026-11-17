@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
msbuild RohanClientHook.sln /p:Configuration=Release /p:Platform=Win32 /m:1 /nr:false /nologo /verbosity:minimal
if errorlevel 1 goto failed
if exist "bin\Autoload\rohanclient_unpacked.exe.json" node tools/sync-autoload.mjs
set "buildResult=%errorlevel%"
popd
exit /b %buildResult%
:failed
set "buildResult=%errorlevel%"
popd
exit /b %buildResult%


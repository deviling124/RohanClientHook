@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\NovaChannelTests" mkdir "obj\NovaChannelTests"
cl /nologo /EHsc /std:c++17 /W4 /WX /Fo:"obj\NovaChannelTests\\" /Fe:"obj\NovaChannelTests\NovaChannelTests.exe" tests\NovaChannelTests.cpp
if errorlevel 1 goto failed
obj\NovaChannelTests\NovaChannelTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

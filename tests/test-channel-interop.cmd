@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\ChannelInteropTests" mkdir "obj\ChannelInteropTests"
rem Uses the sibling RohanMapServ_Hook sources (server engine), like the other cross-project tests.
cl /nologo /EHsc /std:c++17 /W4 /WX /DNOMINMAX /Fo:"obj\ChannelInteropTests\\" /Fe:"obj\ChannelInteropTests\ChannelInteropTests.exe" tests\ChannelInteropTests.cpp
if errorlevel 1 goto failed
obj\ChannelInteropTests\ChannelInteropTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

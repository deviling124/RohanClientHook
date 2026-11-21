@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\InspectTests" mkdir "obj\InspectTests"
cl /nologo /EHsc /std:c++17 /W4 /Fo:"obj\InspectTests\\" /Fe:"obj\InspectTests\InspectClientTests.exe" tests\InspectClientTests.cpp
if errorlevel 1 goto failed
obj\InspectTests\InspectClientTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\EventManagerTests" mkdir "obj\EventManagerTests"
cl /nologo /EHsc /std:c++17 /W4 /Fe:"obj\EventManagerTests\EventManagerClientTests.exe" /Fo:"obj\EventManagerTests\\" tests\EventManagerClientTests.cpp
if errorlevel 1 goto failed
"obj\EventManagerTests\EventManagerClientTests.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\KioskSearchTests" mkdir "obj\KioskSearchTests"
cl /nologo /EHsc /std:c++17 /W4 /Ivendor\detours\src /Fo:"obj\KioskSearchTests\\" /Fe:"obj\KioskSearchTests\KioskSearchTests.exe" tests\KioskSearchTests.cpp obj\RohanClientHook\Release\detours.obj obj\RohanClientHook\Release\modules.obj obj\RohanClientHook\Release\disasm.obj obj\RohanClientHook\Release\image.obj obj\RohanClientHook\Release\creatwth.obj
if errorlevel 1 goto failed
obj\KioskSearchTests\KioskSearchTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

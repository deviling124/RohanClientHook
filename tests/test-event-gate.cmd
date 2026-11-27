@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\EventGateTests" mkdir "obj\EventGateTests"
cl /nologo /EHsc /std:c++17 /W4 /I"vendor\detours\src" /Fe:"obj\EventGateTests\EventGateWallTests.exe" /Fo:"obj\EventGateTests\\" tests\EventGateWallTests.cpp src\EventGate\GateWall.cpp vendor\detours\src\detours.cpp vendor\detours\src\modules.cpp vendor\detours\src\disasm.cpp vendor\detours\src\image.cpp vendor\detours\src\creatwth.cpp
if errorlevel 1 goto failed
"obj\EventGateTests\EventGateWallTests.exe" "00_reference\rohanclient_unpacked.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

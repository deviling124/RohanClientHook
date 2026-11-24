@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\BossCheckTests" mkdir "obj\BossCheckTests"
cl /nologo /EHsc /std:c++17 /W4 /Fo:"obj\BossCheckTests\\" /Fe:"obj\BossCheckTests\BossCheckClientTests.exe" tests\BossCheckClientTests.cpp
if errorlevel 1 goto failed
obj\BossCheckTests\BossCheckClientTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

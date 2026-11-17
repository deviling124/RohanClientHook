@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\Tests" mkdir "obj\Tests"
cl /nologo /EHsc /std:c++17 /W4 /DAHK_WINDOW_MODEL_ONLY /Fo:"obj\Tests\\" /Fe:"obj\Tests\AhkWindowTests.exe" tests\AhkWindowTests.cpp src\AhkWindow.cpp
if errorlevel 1 goto failed
obj\Tests\AhkWindowTests.exe
if errorlevel 1 goto failed
cl /nologo /EHsc /std:c++17 /W4 /Fo:"obj\Tests\\" /Fe:"obj\Tests\StartupTests.exe" tests\StartupTests.cpp
if errorlevel 1 goto failed
obj\Tests\StartupTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

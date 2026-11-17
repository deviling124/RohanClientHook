@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\PriceInputTests" mkdir "obj\PriceInputTests"
cl /nologo /EHsc /std:c++17 /W4 /DPRICE_INPUT_TESTING /Ivendor\detours\src /Fo:"obj\PriceInputTests\\" /Fe:"obj\PriceInputTests\PriceInputTests.exe" tests\PriceInputTests.cpp src\PriceInput.cpp
if errorlevel 1 goto failed
obj\PriceInputTests\PriceInputTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

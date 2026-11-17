@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\EventShopTests" mkdir "obj\EventShopTests"
cl /nologo /EHsc /std:c++17 /W4 /DAHK_WINDOW_MODEL_ONLY /Fo:"obj\EventShopTests\\" /Fe:"obj\EventShopTests\EventShopTests.exe" tests\EventShopTests.cpp src\EventShop\EventShopWindow.cpp ole32.lib
if errorlevel 1 goto failed
obj\EventShopTests\EventShopTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

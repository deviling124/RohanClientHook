@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\BridgeTests" mkdir "obj\BridgeTests"
cl /nologo /EHsc /std:c++17 /W4 /DAHK_BRIDGE_TESTING /DAHK_WINDOW_MODEL_ONLY /I"vendor\detours\src" /Fe:"tests\BridgeTests.exe" /Fo:"obj\BridgeTests\\" tests\BridgeTests.cpp src\Bridge.cpp src\AhkWindow.cpp src\EventShop\EventShopWindow.cpp user32.lib ole32.lib
if errorlevel 1 goto failed
"tests\BridgeTests.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set result=%errorlevel%
popd
exit /b %result%

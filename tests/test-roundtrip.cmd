@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\Tests" mkdir "obj\Tests"
cl /nologo /EHsc /std:c++17 /W4 /DAHK_WINDOW_MODEL_ONLY /Fo:"obj\Tests\\" /Fe:"obj\Tests\ClientServerRoundTrip.exe" tests\ClientServerRoundTrip.cpp src\AhkWindow.cpp ..\RohanMapServ_Hook\src\AdvancedHunterKit\Policy.cpp ..\RohanMapServ_Hook\src\AdvancedHunterKit\UserSettings.cpp
if errorlevel 1 goto failed
obj\Tests\ClientServerRoundTrip.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

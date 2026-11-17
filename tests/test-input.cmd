@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\Tests" mkdir "obj\Tests"
cl /nologo /EHsc /std:c++17 /W4 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Ivendor\imgui /Ivendor\detours\src /Fo:"obj\Tests\\" /Fe:"obj\Tests\InputTests.exe" tests\InputTests.cpp obj\RohanClientHook\Release\Splash.obj obj\RohanClientHook\Release\Dx9Frame.obj obj\RohanClientHook\Release\AhkWindow.obj obj\RohanClientHook\Release\EventShopWindow.obj obj\RohanClientHook\Release\ItemIcons.obj obj\RohanClientHook\Release\Bridge.obj obj\RohanClientHook\Release\GlobalChat.obj obj\RohanClientHook\Release\EventNotice.obj obj\RohanClientHook\Release\AutotradeLogin.obj obj\RohanClientHook\Release\PriceInput.obj obj\RohanClientHook\Release\KioskSearch.obj obj\RohanClientHook\Release\detours.obj obj\RohanClientHook\Release\modules.obj obj\RohanClientHook\Release\disasm.obj obj\RohanClientHook\Release\image.obj obj\RohanClientHook\Release\creatwth.obj obj\RohanClientHook\Release\imgui.obj obj\RohanClientHook\Release\imgui_draw.obj obj\RohanClientHook\Release\imgui_tables.obj obj\RohanClientHook\Release\imgui_widgets.obj obj\RohanClientHook\Release\imgui_impl_win32.obj obj\RohanClientHook\Release\imgui_impl_dx9.obj /link d3d9.lib dinput8.lib dxguid.lib user32.lib gdi32.lib imm32.lib dwmapi.lib gdiplus.lib ole32.lib
if errorlevel 1 goto failed
obj\Tests\InputTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

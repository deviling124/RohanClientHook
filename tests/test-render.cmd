@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\RenderTests" mkdir "obj\RenderTests"
if not exist "bin\Tests" mkdir "bin\Tests"
cl /nologo /EHsc /std:c++17 /W4 /O2 /I"vendor\imgui" /Fo:"obj\RenderTests\\" /Fe:"bin\Tests\RenderStateTests.exe" tests\RenderStateTests.cpp src\Dx9Frame.cpp vendor\imgui\imgui.cpp vendor\imgui\imgui_draw.cpp vendor\imgui\imgui_tables.cpp vendor\imgui\imgui_widgets.cpp vendor\imgui\backends\imgui_impl_dx9.cpp /link d3d9.lib d3dcompiler.lib user32.lib gdi32.lib imm32.lib
if errorlevel 1 goto failed
"bin\Tests\RenderStateTests.exe"
set "testResult=%errorlevel%"
popd
exit /b %testResult%
:failed
set "testResult=%errorlevel%"
popd
exit /b %testResult%

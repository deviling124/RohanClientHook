@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\CostumeClientTests" mkdir "obj\CostumeClientTests"
cl /nologo /EHsc /std:c++17 /W4 /DCOSTUME_PREVIEW_TESTING /I"vendor\detours\src" /Fe:"obj\CostumeClientTests\CostumeClientTests.exe" /Fo:"obj\CostumeClientTests\\" tests\CostumeClientTests.cpp src\CostumeEffect\CostumePreview.cpp vendor\detours\src\detours.cpp vendor\detours\src\modules.cpp vendor\detours\src\disasm.cpp vendor\detours\src\image.cpp vendor\detours\src\creatwth.cpp
if errorlevel 1 goto failed
"obj\CostumeClientTests\CostumeClientTests.exe" "00_reference\rohanclient_unpacked.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

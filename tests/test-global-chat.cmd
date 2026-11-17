@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\GlobalChatTests" mkdir "obj\GlobalChatTests"
cl /nologo /EHsc /std:c++17 /W4 /DGLOBAL_CHAT_TESTING /Ivendor\detours\src /Fo:"obj\GlobalChatTests\\" /Fe:"obj\GlobalChatTests\GlobalChatTests.exe" tests\GlobalChatTests.cpp obj\RohanClientHook\Release\detours.obj obj\RohanClientHook\Release\modules.obj obj\RohanClientHook\Release\disasm.obj obj\RohanClientHook\Release\image.obj obj\RohanClientHook\Release\creatwth.obj
if errorlevel 1 goto failed
obj\GlobalChatTests\GlobalChatTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

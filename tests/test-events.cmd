@echo off
setlocal
pushd "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto failed
if not exist "obj\EventTests" mkdir "obj\EventTests"
cl /nologo /EHsc /std:c++17 /W4 /DEVENT_NOTICE_TESTING /DCUSTOM_EVENTS_TESTING /I"vendor\detours\src" /Fe:"obj\EventTests\EventNoticeTests.exe" /Fo:"obj\EventTests\\" tests\EventNoticeTests.cpp src\CustomEvents\MonsterName.cpp ..\RohanMapServ_Hook\src\CustomEvents\DisguiseEvent.cpp ..\RohanMapServ_Hook\src\CustomEvents\MapTHAdapter.cpp ..\RohanMapServ_Hook\src\CustomEvents\CustomEvents.cpp ..\RohanMapServ_Hook\src\CustomEvents\EventPoints.cpp vendor\detours\src\detours.cpp vendor\detours\src\modules.cpp vendor\detours\src\disasm.cpp vendor\detours\src\image.cpp vendor\detours\src\creatwth.cpp /link odbc32.lib ole32.lib advapi32.lib
if errorlevel 1 goto failed
"obj\EventTests\EventNoticeTests.exe" "00_reference\rohanclient_unpacked.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "result=%errorlevel%"
popd
exit /b %result%

@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist "obj\CollectionBookTests" mkdir "obj\CollectionBookTests"
cl /nologo /EHsc /std:c++17 /W4 /Fo:"obj\CollectionBookTests\\" /Fe:"obj\CollectionBookTests\CollectionBookClientTests.exe" tests\CollectionBookClientTests.cpp src\CollectionBook\CollectionBookClient.cpp
if errorlevel 1 goto failed
obj\CollectionBookTests\CollectionBookClientTests.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1

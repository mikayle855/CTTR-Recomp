@echo off
rem Builds games\cttr\launcher\CTTR.exe (the no-console launcher) from cttr_launcher.cpp + cttr.ico.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rc /nologo /fo cttr_launcher.res cttr_launcher.rc || exit /b 1
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe" /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE ^
  cttr_launcher.cpp cttr_launcher.res /Fe:CTTR.exe /link /SUBSYSTEM:WINDOWS user32.lib comdlg32.lib || exit /b 1
del cttr_launcher.res cttr_launcher.obj 2>nul
echo Built %~dp0CTTR.exe

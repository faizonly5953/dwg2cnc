@echo off
rem Build ..\dist\dwg2cnc.exe (command line) dan ..\dist\dwg2cnc-gui.exe (jendela) dari satu sumber, dengan MSVC.
rem CRT statis (/MT): exe jalan di Windows mana pun tanpa installer.
rem Lokasi Visual Studio lain? set VCVARS=...\VC\Auxiliary\Build\vcvars64.bat lalu jalankan lagi.
setlocal
cd /d "%~dp0"
if not exist ..\dist mkdir ..\dist
if not defined VCVARS set "VCVARS=D:\Software\VSCommunity2022\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul
where cl >nul 2>nul || (echo cl.exe tidak ditemukan. Cek VCVARS. & exit /b 1)
rc /nologo /fo dwg2cnc.res dwg2cnc.rc || exit /b 1
cl /nologo /c /O2 /MT /EHsc /std:c++17 /W3 /utf-8 dwg2cnc.cpp || exit /b 1
link /nologo dwg2cnc.obj dwg2cnc.res /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup /MANIFEST:EMBED /OUT:..\dist\dwg2cnc.exe || exit /b 1
link /nologo dwg2cnc.obj dwg2cnc.res /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:EMBED /OUT:..\dist\dwg2cnc-gui.exe || exit /b 1
del /q dwg2cnc.obj dwg2cnc.res
copy /y dwg2cnc.ini ..\dist\ >nul
echo OK: %~dp0..\dist\dwg2cnc.exe dan dwg2cnc-gui.exe

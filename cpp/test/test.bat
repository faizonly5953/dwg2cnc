@echo off
rem Uji regresi: output dwg2cnc.exe harus sama persis dengan dwg2cnc.py (uji.expected.TXT dibuat oleh Python).
setlocal
cd /d "%~dp0"
set "OUT=%TEMP%\dwg2cnc_test"
"..\..\dist\dwg2cnc.exe" uji.dxf -o "%OUT%" --preview no >nul || (echo GAGAL: konversi error & exit /b 1)
fc /b uji.expected.TXT "%OUT%\uji.TXT" >nul || (echo GAGAL: output beda dari versi Python & exit /b 1)
echo LULUS

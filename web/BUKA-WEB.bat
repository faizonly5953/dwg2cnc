@echo off
setlocal
title Tool DWG ke G-code (web)
set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY ( where python >nul 2>nul && set "PY=python" )
if not defined PY (
  echo Python belum terpasang. Install dari https://www.python.org/downloads/
  pause
  exit /b 1
)
echo Tool web berjalan di http://localhost:8765  (tutup jendela ini untuk berhenti)
start "" http://localhost:8765/index.html
cd /d "%~dp0"
%PY% -m http.server 8765 --bind 127.0.0.1

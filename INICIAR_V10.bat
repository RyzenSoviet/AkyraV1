@echo off
setlocal
cd /d "%~dp0"
title AKYRA V10 // STABLE
py akyra_v10.py
if errorlevel 1 (
  echo.
  echo AKYRA se cerro con un error. Revisa el mensaje de arriba.
  pause
)

@echo off
setlocal
cd /d "%~dp0"
title AKYRA V10 // MODELO DE VOZ
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0DESCARGAR_MODELO_VOZ.ps1"
if errorlevel 1 (
  echo.
  echo [ERROR] No se pudo descargar el modelo. Intenta nuevamente con Internet.
  pause
  exit /b 1
)
echo.
pause

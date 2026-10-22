@echo off
setlocal
cd /d "%~dp0"
title AKYRA V10 // INSTALADOR

echo ==============================================
echo       AKYRA V10 // STABLE INSTALLER
echo ==============================================
echo.
py -m pip install --upgrade pip
if errorlevel 1 goto :error
py -m pip install -r requirements.txt
if errorlevel 1 goto :error

echo.
if exist "voice_models\vosk-model-small-es-0.42\conf" (
  echo [OK] Modelo de voz ya instalado.
) else (
  echo [INFO] Falta el modelo de voz offline en espanol.
  echo Ejecuta DESCARGAR_MODELO_VOZ.bat una vez.
)
echo.
echo Instalacion terminada.
pause
exit /b 0

:error
echo.
echo [ERROR] No se pudieron instalar todas las dependencias.
echo Comprueba Python y tu conexion a Internet.
pause
exit /b 1

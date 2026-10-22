$ErrorActionPreference = 'Stop'
$Base = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Base

$Url = 'https://alphacephei.com/vosk/models/vosk-model-small-es-0.42.zip'
$Zip = Join-Path $Base 'vosk-model-small-es-0.42.zip'
$Dest = Join-Path $Base 'voice_models'
$Model = Join-Path $Dest 'vosk-model-small-es-0.42'

if (Test-Path (Join-Path $Model 'conf')) {
    Write-Host '[OK] El modelo Vosk ya esta instalado.' -ForegroundColor Green
    exit 0
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Write-Host 'Descargando modelo Vosk espanol pequeno (~39 MB)...' -ForegroundColor Cyan
Invoke-WebRequest -Uri $Url -OutFile $Zip
Write-Host 'Extrayendo...' -ForegroundColor Cyan
Expand-Archive -Path $Zip -DestinationPath $Dest -Force
Remove-Item $Zip -Force
Write-Host '[OK] Modelo instalado en voice_models.' -ForegroundColor Green

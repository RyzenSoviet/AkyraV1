AKYRA V10 // STABLE VOICE
==========================

BASE
- Construida directamente sobre AKYRA V9.5 STABLE.
- Conserva el mismo pipeline de camara + MediaPipe + voz que funcionaba fluido.
- No incorpora la arquitectura experimental de las antiguas V10/V10.1.

QUE CAMBIA
- Mantiene wake word: AKYRA.
- Amplia bastante los comandos de voz.
- Ventanas: Alt+Tab, Win+Tab y escritorio.
- Edicion: copiar, pegar, cortar, deshacer y rehacer.
- Navegador: pestanas, zoom y actualizar.
- Mouse: clic, doble clic y clic derecho por voz.
- Navegacion: flechas, Enter, Escape, Page Up/Down.
- Multimedia y targets USB/BLE/BOTH se mantienen.

RENDIMIENTO
Se conservaron los parametros y la estructura de V9.5:
- Camara 640x360, objetivo 60 FPS y MJPG.
- Vision interna 480x270.
- Voz Vosk en hilo independiente.
- Sin pipeline asincrono experimental de V10.1.

USO
1. Si ya tienes el modelo Vosk en tu V9.5, copia la carpeta voice_models a esta carpeta.
2. Si no, ejecuta DESCARGAR_MODELO_VOZ.bat.
3. Ejecuta INSTALAR_V10.bat solo si faltan dependencias.
4. Ejecuta INICIAR_V10.bat.

No es necesario reflashear el ESP32 si el firmware que usabas con V9.5 ya
aceptaba las acciones HID de V8.5/V9.

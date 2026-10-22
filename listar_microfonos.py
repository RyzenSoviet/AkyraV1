try:
    import sounddevice as sd
except Exception as exc:
    raise SystemExit(f"Falta sounddevice: {exc}\nEjecuta INSTALAR_V10.bat")

print("\n=== AKYRA V10 // MICROFONOS ===\n")
print(sd.query_devices())
print("\nEl microfono predeterminado puede dejarse como AUTO/null en config.json.")
input("\nEnter para cerrar...")

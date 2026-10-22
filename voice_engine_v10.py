from pathlib import Path
import json
import queue
import re
import threading
import time
import unicodedata

try:
    import numpy as np
except Exception:
    np = None

try:
    import sounddevice as sd
except Exception:
    sd = None

try:
    from vosk import Model, KaldiRecognizer, SetLogLevel
except Exception:
    Model = KaldiRecognizer = SetLogLevel = None

try:
    import winsound
except Exception:
    winsound = None


def normalize_text(text: str) -> str:
    text = (text or "").lower().strip()
    text = unicodedata.normalize("NFD", text)
    text = "".join(ch for ch in text if unicodedata.category(ch) != "Mn")
    text = re.sub(r"[^a-z0-9ñ\s]", " ", text)
    return " ".join(text.split())


class VoiceEngine:
    """Offline Spanish voice recognizer for AKYRA V10.

    The engine never touches gesture state directly. It only pushes recognized
    commands to a thread-safe queue so the main loop remains deterministic.
    """

    def __init__(self, config: dict, base_dir: Path, command_queue: queue.Queue):
        self.cfg = config or {}
        self.base_dir = Path(base_dir)
        self.command_queue = command_queue
        self.audio_queue = queue.Queue(maxsize=24)
        self.stop_event = threading.Event()
        self.thread = None

        self.enabled = bool(self.cfg.get("enabled", True))
        self.status = "BOOT"
        self.partial = ""
        self.last_text = ""
        self.last_command = ""
        self.last_error = ""
        self.last_event_t = time.monotonic()
        self.level = 0.0

        self.sample_rate = int(self.cfg.get("sample_rate", 16000))
        self.device = self.cfg.get("device", None)
        self.timeout = float(self.cfg.get("command_timeout", 4.5))
        self.wake_words = [normalize_text(x) for x in self.cfg.get(
            "wake_words", ["akyra", "akira", "a kira", "aquira", "a kyra"]
        )]

        model_rel = str(self.cfg.get(
            "model_path", "voice_models/vosk-model-small-es-0.42"
        ))
        model_path = Path(model_rel)
        if not model_path.is_absolute():
            model_path = self.base_dir / model_path
        self.model_path = model_path

        self._listening_until = 0.0
        self._wake_latched = False
        self._manual_listen = False
        self._meter_tick = 0

    def _beep(self, kind="wake"):
        if not self.cfg.get("audio_feedback", True) or winsound is None:
            return
        try:
            freq = 930 if kind == "wake" else 1240 if kind == "ok" else 520
            dur = 55 if kind != "error" else 90
            winsound.Beep(freq, dur)
        except Exception:
            pass

    def _set_status(self, value):
        self.status = value
        self.last_event_t = time.monotonic()

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._run, daemon=True, name="AKYRA-VOICE")
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=1.2)

    def toggle(self):
        self.enabled = not self.enabled
        if self.enabled:
            self._set_status("READY")
        else:
            self._set_status("OFF")
            self.partial = ""
            self._listening_until = 0.0
        return self.enabled

    def manual_listen(self):
        if not self.enabled:
            self.enabled = True
        self._manual_listen = True
        self._listening_until = time.monotonic() + self.timeout
        self.partial = ""
        self._set_status("LISTENING")
        self._beep("wake")

    def snapshot(self):
        return {
            "enabled": self.enabled,
            "status": self.status,
            "partial": self.partial,
            "last_text": self.last_text,
            "last_command": self.last_command,
            "error": self.last_error,
            "level": self.level,
            "model_ok": self.model_path.is_dir(),
        }

    def _has_wake(self, text):
        n = normalize_text(text)
        return any(w and w in n for w in self.wake_words)

    def _after_wake(self, text):
        n = normalize_text(text)
        for w in sorted(self.wake_words, key=len, reverse=True):
            pos = n.find(w)
            if pos >= 0:
                return n[pos + len(w):].strip()
        return n

    def _emit_command(self, text):
        text = normalize_text(text)
        if not text:
            return False
        self.last_text = text
        self.last_command = text
        try:
            self.command_queue.put_nowait(text)
        except queue.Full:
            try:
                self.command_queue.get_nowait()
            except queue.Empty:
                pass
            try:
                self.command_queue.put_nowait(text)
            except queue.Full:
                return False
        self.partial = ""
        self._listening_until = 0.0
        self._manual_listen = False
        self._wake_latched = False
        self._set_status("EXEC")
        self._beep("ok")
        return True

    def _handle_text(self, text, is_final=False):
        text = normalize_text(text)
        if not text:
            return

        now = time.monotonic()
        if not self.enabled:
            return

        listening = self._listening_until > now

        if not listening:
            if self._has_wake(text):
                self._wake_latched = True
                self._listening_until = now + self.timeout
                self._set_status("LISTENING")
                self._beep("wake")
                remainder = self._after_wake(text)
                if is_final and remainder:
                    self._emit_command(remainder)
            return

        # Already listening. If the recognizer finalizes the complete phrase,
        # strip the wake word if it is still present and execute what follows.
        if is_final:
            command = self._after_wake(text) if self._has_wake(text) else text
            if command:
                self._emit_command(command)
        else:
            self.partial = self._after_wake(text) if self._has_wake(text) else text

    def _audio_callback(self, indata, frames, time_info, status):
        if self.stop_event.is_set():
            return
        try:
            raw = bytes(indata)

            # V10: the meter is UI-only. Update it at half rate and use NumPy
            # when available so Python does not loop through hundreds of samples.
            self._meter_tick = (self._meter_tick + 1) & 1
            if raw and self._meter_tick == 0:
                if np is not None:
                    samples = np.frombuffer(raw, dtype=np.int16)
                    if samples.size > 640:
                        samples = samples[::max(1, samples.size // 640)]
                    if samples.size:
                        avg = float(np.abs(samples.astype(np.int32)).mean())
                        self.level = min(1.0, avg / 4200.0)
                else:
                    # Small fallback sample if NumPy is unavailable.
                    sample_count = min(len(raw) // 2, 160)
                    if sample_count:
                        total = 0
                        step = max(1, (len(raw) // 2) // sample_count)
                        used = 0
                        for i in range(0, sample_count * step, step):
                            j = i * 2
                            if j + 1 >= len(raw):
                                break
                            total += abs(int.from_bytes(raw[j:j+2], "little", signed=True))
                            used += 1
                        if used:
                            self.level = min(1.0, (total / used) / 4200.0)

            self.audio_queue.put_nowait(raw)
        except queue.Full:
            # Keep newest audio instead of letting a backlog grow.
            try:
                self.audio_queue.get_nowait()
                self.audio_queue.put_nowait(raw)
            except Exception:
                pass
        except Exception:
            pass

    def _run(self):
        if sd is None or Model is None or KaldiRecognizer is None:
            self.last_error = "Faltan sounddevice/vosk. Ejecuta INSTALAR_V10.bat"
            self._set_status("ERROR")
            return

        if not self.model_path.is_dir():
            self.last_error = "Falta modelo Vosk. Ejecuta DESCARGAR_MODELO_VOZ.bat"
            self._set_status("MODEL")
            return

        try:
            if SetLogLevel is not None:
                SetLogLevel(-1)
            model = Model(str(self.model_path))
            recognizer = KaldiRecognizer(model, self.sample_rate)
            recognizer.SetWords(False)
        except Exception as exc:
            self.last_error = f"Modelo de voz: {exc}"
            self._set_status("ERROR")
            return

        stream_kwargs = {
            "samplerate": self.sample_rate,
            "blocksize": 4000,
            "dtype": "int16",
            "channels": 1,
            "callback": self._audio_callback,
        }
        if self.device not in (None, "", "AUTO"):
            try:
                stream_kwargs["device"] = int(self.device)
            except Exception:
                stream_kwargs["device"] = self.device

        try:
            with sd.RawInputStream(**stream_kwargs):
                self._set_status("READY" if self.enabled else "OFF")
                while not self.stop_event.is_set():
                    now = time.monotonic()
                    if self.enabled and self._listening_until and now > self._listening_until:
                        self._listening_until = 0.0
                        self._manual_listen = False
                        self._wake_latched = False
                        self.partial = ""
                        self._set_status("READY")

                    # EXEC is a short HUD state, then returns to READY.
                    if self.enabled and self.status == "EXEC" and now - self.last_event_t > 0.75:
                        self._set_status("READY")

                    try:
                        data = self.audio_queue.get(timeout=0.12)
                    except queue.Empty:
                        continue

                    if not self.enabled:
                        continue

                    if recognizer.AcceptWaveform(data):
                        try:
                            text = json.loads(recognizer.Result()).get("text", "")
                        except Exception:
                            text = ""
                        self._handle_text(text, is_final=True)
                    else:
                        try:
                            partial = json.loads(recognizer.PartialResult()).get("partial", "")
                        except Exception:
                            partial = ""
                        if self.status == "LISTENING":
                            self.partial = self._after_wake(partial) if self._has_wake(partial) else normalize_text(partial)
                        self._handle_text(partial, is_final=False)
        except Exception as exc:
            self.last_error = f"Microfono: {exc}"
            self._set_status("ERROR")
            self._beep("error")

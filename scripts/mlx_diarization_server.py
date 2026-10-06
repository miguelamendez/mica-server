"""HTTP adapter for mlx-audio's diarization library (not speech recognition)."""
import argparse
import dataclasses
import os
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--memory-gib", type=float, required=True)
    args = parser.parse_args()
    import mlx.core as mx
    from mlx_worker import configure_memory
    configure_memory(mx, args.memory_gib)
    from fastapi import FastAPI, File, Form, HTTPException, UploadFile
    import uvicorn
    from mlx_audio.vad import load
    model = load(args.model, strict=True)
    lock = threading.Lock()
    app = FastAPI()

    @app.get("/health")
    def health():
        return {"status": "ok"}

    @app.post("/v1/audio/diarizations")
    def diarize(file: UploadFile = File(...), model_id: str = Form("", alias="model"),
                stream: bool = Form(False)):
        if stream:
            raise HTTPException(400, "this adapter exposes offline diarization; streaming is not certified")
        payload = file.file.read(16 * 1024**2 + 1)
        if len(payload) > 16 * 1024**2:
            raise HTTPException(413, "audio upload exceeds 16 MiB")
        if not (payload.startswith(b"RIFF") and payload[8:12] == b"WAVE"):
            raise HTTPException(400, "provide a 16 kHz mono WAV")
        import io
        import wave
        try:
            with wave.open(io.BytesIO(payload)) as audio:
                if audio.getframerate() != 16000 or audio.getnchannels() != 1:
                    raise HTTPException(400, "provide a 16 kHz mono WAV")
                if audio.getnframes() / 16000 > 60:
                    raise HTTPException(413, "offline adapter limits each clip to 60 seconds")
        except wave.Error as error:
            raise HTTPException(400, "invalid PCM WAV") from error
        path = None
        try:
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as output:
                output.write(payload)
                path = output.name
            with lock:
                result = model.generate(path)
                turns = []
                for segment in result.segments:
                    value = dataclasses.asdict(segment) if dataclasses.is_dataclass(segment) else vars(segment)
                    turns.append({"start": float(value["start"]), "end": float(value["end"]),
                                  "speaker_id": "speaker_" + str(value["speaker"]),
                                  **({"confidence": float(value["confidence"])} if "confidence" in value else {})})
                mx.clear_cache()
            return {"model": model_id, "speaker_turns": turns, "timestamp_unit": "seconds"}
        finally:
            if path is not None:
                os.unlink(path)

    uvicorn.run(app, host="127.0.0.1", port=args.port)


if __name__ == "__main__":
    main()

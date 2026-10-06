"""Real inference smoke test; standard library only, never prints credentials.

This validates the API shape and timing, NOT a labeled diarization error rate.
"""
import argparse
import json
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:9296")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    key = (args.root / "secrets/api-key").read_text().strip()
    with wave.open(str(args.audio)) as audio:
        duration = audio.getnframes() / audio.getframerate()
    boundary = "mica-diarization-smoke"
    content = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
               "nemotron-3-diarization\r\n"
               f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"sample.wav\"\r\n"
               "Content-Type: audio/wav\r\n\r\n").encode()
    content += args.audio.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    request = urllib.request.Request(args.url + "/v1/audio/diarizations", data=content,
        headers={"Authorization": "Bearer " + key,
                 "Content-Type": "multipart/form-data; boundary=" + boundary})
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(error.read().decode()) from error
    elapsed = time.perf_counter() - started
    assert result["timestamp_unit"] == "seconds"
    assert result["speaker_turns"], "speech fixture returned no turns"
    for turn in result["speaker_turns"]:
        assert 0 <= turn["start"] < turn["end"] <= duration + 0.02
        assert isinstance(turn["speaker_id"], str)
    report = {"audio_seconds": duration, "request_seconds": elapsed,
              "real_time_factor": elapsed / duration, "result": result,
              "validation": "nonempty turns, timestamp bounds and API shape; no DER claim"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

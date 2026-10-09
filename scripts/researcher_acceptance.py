#!/usr/bin/env python3
"""Real inference acceptance for the local-researcher-gguf workload.

Uses only Python's standard library as an offline test client, not a runtime
dependency. Saves full responses, vectors, timing, accuracy checks and worker
snapshots after each call, so failures cannot be mistaken for completed tests.
"""
import argparse
import base64
import hashlib
import json
import math
import mimetypes
from pathlib import Path
import time
import urllib.error
import urllib.request
import wave


def uri(path):
    return "data:" + (mimetypes.guess_type(path.name)[0] or "application/octet-stream") + ";base64," + base64.b64encode(path.read_bytes()).decode()


def cosine(a, b):
    return sum(x*y for x, y in zip(a, b)) / math.sqrt(sum(x*x for x in a) * sum(x*x for x in b))


def audio_fixture(path):
    with wave.open(str(path), "rb") as audio:
        if (audio.getnchannels() != 1 or audio.getframerate() != 16000 or
                audio.getsampwidth() != 2 or audio.getnframes() < 16000):
            raise ValueError("audio must contain at least one second of mono 16 kHz PCM16 speech")
        return {"duration_seconds": audio.getnframes() / audio.getframerate(),
                "sample_rate": audio.getframerate(), "channels": audio.getnchannels()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base-url", default="http://127.0.0.1:8096")
    p.add_argument("--api-key-file", type=Path, required=True)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--video", type=Path, required=True)
    p.add_argument("--audio", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--only", choices=("all", "embeddings", "generation"), default="all")
    p.add_argument("--ready-timeout", type=int, default=1800)
    args = p.parse_args()
    # Empty audio can look like a decoder failure and invalidate the comparison.
    try:
        audio_info = audio_fixture(args.audio)
    except (ValueError, wave.Error, EOFError, OSError) as error:
        p.error(str(error))
    key = args.api_key_file.read_text().strip()
    report = {"schema": 1, "test_type": "real-inference-smoke-not-benchmark", "scope": args.only, "cases": []}
    report["fixtures"] = {name: {"name": path.name,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for name, path in (("image", args.image), ("video", args.video), ("audio", args.audio))}
    report["fixtures"]["audio"].update(audio_info)

    def sanitized(value):
        if isinstance(value, dict):
            return {k: sanitized(v) for k, v in value.items()}
        if isinstance(value, list):
            return [sanitized(v) for v in value]
        if isinstance(value, str):
            return value.replace(key, "[REDACTED]").replace(str(Path(__file__).resolve().parents[1]), "$REPO").replace(str(Path.home() / ".mica"), "$MICA_HOME").replace(str(Path.home()), "$HOME")
        return value

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(sanitized(report), indent=2) + "\n")

    def call(path, payload=None, auth=True):
        headers = {"Content-Type": "application/json"}
        if auth:
            headers["Authorization"] = "Bearer " + key
        request = urllib.request.Request(args.base_url.rstrip("/") + path,
            data=None if payload is None else json.dumps(payload).encode(), headers=headers)
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(request, timeout=600) as response:
                return response.status, json.load(response), time.perf_counter() - started
        except urllib.error.HTTPError as error:
            return error.code, json.load(error), time.perf_counter() - started
        except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError) as error:
            return 0, {"error": {"message": str(error)}}, time.perf_counter() - started

    def test(name, path, payload, check):
        if (args.only == "embeddings" and name.startswith("gemma-")) or (args.only == "generation" and name.startswith("embedding-")):
            return {}
        status, response, seconds = call(path, payload)
        success = status == 200 and check(response)
        _, snapshot, _ = call("/admin/models")
        workers = snapshot.get("workers", [])
        expected = "gemma4-12b@" if name.startswith("gemma-") else "embeddinggemma-2@"
        residency_ok = len(workers) == 1 and workers[0].get("id", "").startswith(expected)
        success = success and residency_ok
        report["cases"].append({"name": name, "http_status": status, "seconds": seconds,
            "passed": success, "single_expected_worker": residency_ok,
            "response": response, "worker_snapshot": snapshot})
        save()
        print(f"{name}: HTTP {status}, passed={success}, {seconds:.2f}s", flush=True)
        return response

    deadline = time.monotonic() + args.ready_timeout
    while True:
        code, ready, _ = call("/ready", auth=False)
        if code == 200 and ready.get("ready"):
            break
        message = ready.get("error", {}).get("message", "")
        if message or time.monotonic() >= deadline:
            report["readiness_failure"] = message or "readiness timeout"
            report["passed"] = False
            save()
            return 1
        time.sleep(2)

    status, _, _ = call("/v1/embeddings", {"input": "test"}, auth=False)
    report["authentication_rejected"] = status == 401

    def content(response):
        return response.get("choices", [{}])[0].get("message", {}).get("content", "").lower()

    def chat(parts):
        return {"model": "gemma4-12b", "messages": [{"role": "user", "content": parts}],
            "max_tokens": 128, "temperature": 0, "stream": False,
            "chat_template_kwargs": {"enable_thinking": False}}

    test("gemma-text", "/v1/chat/completions", chat("What is 17 plus 25? Answer only the number."), lambda r: "42" in content(r))
    test("gemma-image", "/v1/chat/completions", chat([
        {"type": "image_url", "image_url": {"url": uri(args.image)}},
        {"type": "text", "text": "Read the large central text exactly and name the two square colors."}]),
        lambda r: all(x in content(r) for x in ("mica", "31415", "red", "blue")))
    test("gemma-video", "/v1/chat/completions", chat([
        {"type": "input_video", "input_video": {"data": uri(args.video)}},
        {"type": "text", "text": "Describe the colors shown in chronological order. Be concise."}]),
        lambda r: all(x in content(r) for x in ("red", "blue", "green")) and
            content(r).index("red") < content(r).index("green") < content(r).index("blue"))
    test("gemma-audio", "/v1/chat/completions", chat([
        {"type": "text", "text": "Transcribe the speech. Output only the words you hear."},
        {"type": "input_audio", "input_audio": {"data": base64.b64encode(args.audio.read_bytes()).decode(), "format": "wav"}}]),
        lambda r: all(x in content(r) for x in ("blue", "sky", "green", "grass")))

    def vectors_valid(r, count):
        data = r.get("data", [])
        return len(data) == count and all(len(x.get("embedding", [])) == 768 and
            all(isinstance(v, (int, float)) and math.isfinite(v) for v in x["embedding"]) and
            abs(sum(v*v for v in x["embedding"]) - 1) < .02 for x in data)

    # Three items in one HTTP request exercises native batch input handling.
    result = test("embedding-text-batch", "/v1/embeddings", {
        "model": "embeddinggemma-2", "encoding_format": "float", "input": [
            "task: search result | query: Why is the sky blue?",
            "title: none | text: The sky appears blue because molecules scatter blue sunlight more strongly.",
            "title: none | text: This recipe explains how to bake a chocolate cake."]}, lambda r: vectors_valid(r, 3))
    if vectors_valid(result, 3):
        vectors = [item["embedding"] for item in sorted(result["data"], key=lambda x: x["index"])]
        good, bad = cosine(vectors[0], vectors[1]), cosine(vectors[0], vectors[2])
        report["retrieval"] = {"relevant_cosine": good, "unrelated_cosine": bad, "passed": good > bad}
        save()
    for name, part in (
        ("image", {"type": "image_url", "image_url": {"url": uri(args.image)}}),
        ("video", {"type": "input_video", "input_video": {"data": uri(args.video)}}),
        ("audio", {"type": "input_audio", "input_audio": {"data": base64.b64encode(args.audio.read_bytes()).decode(), "format": "wav"}}),
    ):
        test("embedding-" + name, "/v1/embeddings", {"model": "embeddinggemma-2",
            "input": [{"content": [part]}], "encoding_format": "float"}, lambda r: vectors_valid(r, 1))

    # Confirm sequential eviction and reloading also works in the reverse direction.
    test("gemma-reload-after-embeddings", "/v1/chat/completions", chat("What is 6 times 7? Answer only the number."), lambda r: "42" in content(r))
    negative_cases = (("stream-rejected", {"input": "hello", "stream": True}),
                          ("dimensions-rejected", {"input": "hello", "dimensions": 128}),
                          ("empty-input-rejected", {"input": []}),
                          ("over-budget-rejected", {"input": "sky " * 10000}))
    for name, payload in (() if args.only == "generation" else negative_cases):
        status, response, _ = call("/v1/embeddings", {"model": "embeddinggemma-2", **payload})
        report["cases"].append({"name": name, "http_status": status, "passed": status == 400, "response": response})
        save()
    report["passed"] = report["authentication_rejected"] and all(c["passed"] for c in report["cases"]) and (args.only == "generation" or report.get("retrieval", {}).get("passed", False))
    save()
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

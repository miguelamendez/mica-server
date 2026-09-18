#!/usr/bin/env python3
"""Validate Mica's complete public HTTP surface with real inference.

The client uses only the Python standard library. Capabilities are optional so
the same runner can validate text-only vLLM profiles and full MLX/GGUF catalog
profiles. Every configured inference call must return non-empty, parseable
content; TTS must return a RIFF/WAVE payload.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import json
import mimetypes
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path


def auth_headers(api_key: str, content_type: str | None = None) -> dict[str, str]:
    headers = {"Authorization": f"Bearer {api_key}"}
    if content_type:
        headers["Content-Type"] = content_type
    return headers


def request(url: str, headers: dict[str, str] | None = None, data: bytes | None = None,
            timeout: int = 1800) -> tuple[int, dict[str, str], bytes]:
    req = urllib.request.Request(url, data=data, headers=headers or {},
                                 method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def json_request(base: str, path: str, api_key: str, payload: dict,
                 timeout: int) -> tuple[int, dict, dict[str, str]]:
    status, headers, body = request(
        base + path, auth_headers(api_key, "application/json"),
        json.dumps(payload).encode(), timeout,
    )
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{path} returned invalid JSON ({status}): {error}") from error
    return status, parsed, headers


def data_uri(path: Path) -> str:
    kind = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    return f"data:{kind};base64,{base64.b64encode(path.read_bytes()).decode()}"


def multipart_audio(model: str, audio: Path) -> tuple[str, bytes]:
    boundary = "mica-" + uuid.uuid4().hex
    kind = mimetypes.guess_type(audio.name)[0] or "audio/wav"
    body = (
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
        f"{model}\r\n--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
        f"filename=\"{audio.name}\"\r\nContent-Type: {kind}\r\n\r\n"
    ).encode() + audio.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    return f"multipart/form-data; boundary={boundary}", body


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def record(cases: list[dict], name: str, started: float, status: int,
           detail: dict | None = None) -> None:
    cases.append({"name": name, "status": status,
                  "seconds": time.perf_counter() - started,
                  "passed": 200 <= status < 300, "detail": detail or {}})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--api-key-file", type=Path, required=True)
    parser.add_argument("--text-model")
    parser.add_argument("--asr-model")
    parser.add_argument("--tts-model")
    parser.add_argument("--vision-model")
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--video", type=Path)
    parser.add_argument("--batch-size", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    key = args.api_key_file.read_text().strip()
    cases: list[dict] = []

    started = time.perf_counter()
    status, _, body = request(base + "/health", timeout=args.timeout)
    require(status == 200 and json.loads(body)["status"] == "ok", "/health failed")
    record(cases, "health", started, status)

    started = time.perf_counter()
    status, _, body = request(base + "/ready", timeout=args.timeout)
    require(status == 200 and json.loads(body)["ready"] is True, "/ready failed")
    record(cases, "ready", started, status)

    started = time.perf_counter()
    status, _, _ = request(base + "/v1/models", timeout=args.timeout)
    require(status == 401, "protected endpoint accepted an unauthenticated request")
    cases.append({"name": "authentication-rejection", "status": status,
                  "seconds": time.perf_counter() - started, "passed": True})

    for path, name in (("/v1/models", "models"), ("/admin/models", "admin-models")):
        started = time.perf_counter()
        status, _, body = request(base + path, auth_headers(key), timeout=args.timeout)
        parsed = json.loads(body)
        require(status == 200, f"{path} failed: {parsed}")
        record(cases, name, started, status,
               {"model_count": len(parsed.get("data", parsed.get("workers", [])))})

    if args.text_model:
        chat = {"model": args.text_model,
                "messages": [{"role": "user", "content": "Reply exactly: MICA_ENDPOINT_OK"}],
                "temperature": 0, "max_tokens": 128, "stream": False}
        started = time.perf_counter()
        status, parsed, _ = json_request(base, "/v1/chat/completions", key, chat, args.timeout)
        content = parsed.get("choices", [{}])[0].get("message", {}).get("content", "")
        require(status == 200 and content, f"chat completion failed: {parsed}")
        record(cases, "chat-completions", started, status,
               {"characters": len(content)})

        completion = {"model": args.text_model, "prompt": "Reply exactly: MICA_LEGACY_OK",
                      "temperature": 0, "max_tokens": 128, "stream": False}
        started = time.perf_counter()
        status, parsed, _ = json_request(base, "/v1/completions", key, completion, args.timeout)
        text = parsed.get("choices", [{}])[0].get("text", "")
        require(status == 200 and text, f"legacy completion failed: {parsed}")
        record(cases, "completions", started, status, {"characters": len(text)})

        def batch_call(lane: int) -> int:
            payload = dict(chat)
            payload["messages"] = [{"role": "user",
                                    "content": f"Reply exactly: BATCH_{lane}"}]
            code, result, _ = json_request(
                base, "/v1/chat/completions", key, payload, args.timeout
            )
            require(code == 200 and result.get("choices"), f"batch lane {lane} failed")
            return code

        started = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.batch_size) as executor:
            statuses = list(executor.map(batch_call, range(args.batch_size)))
        record(cases, "parallel-chat-batch", started, min(statuses),
               {"batch_size": args.batch_size})

    if args.asr_model:
        require(args.audio is not None, "--audio is required with --asr-model")
        content_type, body = multipart_audio(args.asr_model, args.audio)
        started = time.perf_counter()
        status, _, response = request(
            base + "/v1/audio/transcriptions", auth_headers(key, content_type), body,
            args.timeout,
        )
        parsed = json.loads(response)
        require(status == 200 and parsed.get("text"), f"ASR failed: {parsed}")
        record(cases, "audio-transcriptions", started, status,
               {"characters": len(parsed["text"])})

    if args.tts_model:
        started = time.perf_counter()
        status, _, body = request(
            base + "/v1/audio/speech", auth_headers(key, "application/json"),
            json.dumps({"model": args.tts_model,
                        "input": "Mica endpoint acceptance preserves clear word boundaries.",
                        "response_format": "wav"}).encode(), args.timeout,
        )
        require(status == 200 and len(body) > 44 and body[:4] == b"RIFF" and
                body[8:12] == b"WAVE", "TTS did not return a valid WAV")
        record(cases, "audio-speech", started, status, {"bytes": len(body)})

    if args.vision_model:
        require(args.image is not None, "--image is required with --vision-model")
        media_cases = [("image-chat",
                        [{"type": "image_url",
                          "image_url": {"url": data_uri(args.image)}},
                         {"type": "text",
                          "text": "Read the large text and describe the colored regions."}])]
        if args.video:
            media_cases.append(
                ("video-chat",
                 [{"type": "input_video",
                   "input_video": {"data": data_uri(args.video)}},
                  {"type": "text",
                   "text": "Describe the color sequence and ordinal text in order."}])
            )
        for case_name, media in media_cases:
            started = time.perf_counter()
            status, parsed, _ = json_request(
                base, "/v1/chat/completions", key,
                {"model": args.vision_model,
                 "messages": [{"role": "user", "content": media}],
                 "temperature": 0, "max_tokens": 512, "stream": False}, args.timeout,
            )
            content = parsed.get("choices", [{}])[0].get("message", {}).get("content", "")
            require(status == 200 and content, f"{case_name} failed: {parsed}")
            record(cases, case_name, started, status, {"characters": len(content)})

    report = {"schema": 1, "base_url": base,
              "passed": all(case["passed"] for case in cases), "cases": cases}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

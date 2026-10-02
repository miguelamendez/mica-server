#!/usr/bin/env python3
"""Exercise continuous batching through Mica's public OpenAI-compatible routes."""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import json
import mimetypes
import statistics
import time
import urllib.request
import uuid
from pathlib import Path


def csv_ints(value: str) -> list[int]:
    result = [int(item.strip()) for item in value.split(",") if item.strip()]
    if not result or any(item < 1 for item in result):
        raise argparse.ArgumentTypeError("batch sizes must be positive")
    return result


def data_uri(path: Path) -> str:
    kind = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    return f"data:{kind};base64,{base64.b64encode(path.read_bytes()).decode()}"


def headers(api_key: str, content_type: str) -> dict[str, str]:
    result = {"Content-Type": content_type}
    if api_key:
        result["Authorization"] = f"Bearer {api_key}"
    return result


def post_json(url: str, api_key: str, payload: dict, timeout: int) -> tuple[bytes, str]:
    request = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers=headers(api_key, "application/json"), method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read(), response.headers.get_content_type()


def post_transcription(url: str, api_key: str, model: str, audio: Path,
                       timeout: int) -> tuple[bytes, str]:
    boundary = "mica-" + uuid.uuid4().hex
    kind = mimetypes.guess_type(audio.name)[0] or "audio/wav"
    chunks = []
    for name, value in (("model", model),):
        chunks.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n"
            f"{value}\r\n".encode()
        )
    chunks.append(
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
        f"filename=\"{audio.name}\"\r\nContent-Type: {kind}\r\n\r\n".encode()
    )
    chunks.extend((audio.read_bytes(), b"\r\n", f"--{boundary}--\r\n".encode()))
    request = urllib.request.Request(
        url, data=b"".join(chunks),
        headers=headers(api_key, f"multipart/form-data; boundary={boundary}"),
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read(), response.headers.get_content_type()


def make_call(args: argparse.Namespace, lane: int):
    base = args.base_url.rstrip("/")
    if args.capability == "asr":
        return lambda: post_transcription(
            base + "/v1/audio/transcriptions", args.api_key, args.model,
            args.audio, args.timeout,
        )
    if args.capability == "tts":
        payload = {
            "model": args.model,
            "input": f"{args.prompt} Batch lane {lane + 1}.",
            "voice": args.voice,
            "response_format": "wav",
        }
        return lambda: post_json(
            base + "/v1/audio/speech", args.api_key, payload, args.timeout
        )
    content: str | list[dict] = f"{args.prompt} Batch lane {lane + 1}."
    if args.capability == "vision":
        media = []
        if args.image:
            media.append({"type": "image_url", "image_url": {"url": data_uri(args.image)}})
        if args.video:
            media.append({"type": "input_video", "input_video": {"data": data_uri(args.video)}})
        content = media + [{"type": "text", "text": content}]
    payload = {
        "model": args.model,
        "messages": [{"role": "user", "content": content}],
        "temperature": 0,
        "max_tokens": args.max_tokens,
        "stream": False,
    }
    return lambda: post_json(
        base + "/v1/chat/completions", args.api_key, payload, args.timeout
    )


def timed_call(call):
    started = time.perf_counter()
    body, content_type = call()
    return time.perf_counter() - started, body, content_type


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--api-key", default="")
    parser.add_argument("--api-key-file", type=Path)
    parser.add_argument("--model", required=True)
    parser.add_argument("--capability", choices=("text", "vision", "asr", "tts"), required=True)
    parser.add_argument("--batch-sizes", type=csv_ints, default=csv_ints("1,2,4"))
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--prompt", default="Respond concisely and preserve word boundaries.")
    parser.add_argument("--voice", default="default")
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--video", type=Path)
    parser.add_argument("--max-tokens", type=int, default=128)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.api_key_file:
        args.api_key = args.api_key_file.read_text().strip()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.capability == "asr" and not args.audio:
        parser.error("ASR batch tests require --audio")
    if args.capability == "vision" and not (args.image or args.video):
        parser.error("vision batch tests require --image and/or --video")

    cases = []
    for width in args.batch_sizes:
        latencies = []
        byte_counts = []
        content_types = []
        for _ in range(args.runs):
            calls = [make_call(args, lane) for lane in range(width)]
            started = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=width) as executor:
                futures = [executor.submit(timed_call, call) for call in calls]
                for future in futures:
                    latency, body, content_type = future.result()
                    latencies.append(latency)
                    byte_counts.append(len(body))
                    content_types.append(content_type)
            wall = time.perf_counter() - started
        cases.append({
            "batch_size": width,
            "runs": args.runs,
            "requests": width * args.runs,
            "last_run_wall_seconds": wall,
            "latency_seconds": {
                "min": min(latencies),
                "median": statistics.median(latencies),
                "max": max(latencies),
            },
            "response_bytes": byte_counts,
            "content_types": sorted(set(content_types)),
            "all_nonempty": all(size > 0 for size in byte_counts),
        })
    report = {
        "schema": 1,
        "model": args.model,
        "capability": args.capability,
        "batch_semantics": "parallel online requests for continuous batching",
        "cases": cases,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

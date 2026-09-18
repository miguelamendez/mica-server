#!/usr/bin/env python3
"""Run deterministic image and video smoke tests against an OpenAI chat route."""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import time
import urllib.request
from pathlib import Path


def data_uri(path: Path) -> str:
    media_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    return f"data:{media_type};base64,{base64.b64encode(path.read_bytes()).decode()}"


def request_chat(base_url: str, payload: dict) -> tuple[dict, float]:
    request = urllib.request.Request(
        base_url.rstrip("/") + "/v1/chat/completions",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    start = time.perf_counter()
    with urllib.request.urlopen(request, timeout=900) as response:
        body = json.load(response)
    return body, (time.perf_counter() - start) * 1000


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-tokens", type=int, default=256)
    args = parser.parse_args()

    cases = [
        (
            "image",
            "image_url",
            {"url": data_uri(args.image)},
            "Read the exact large text and describe where the blue and red regions are. Be concise.",
        ),
        (
            "video",
            "input_video",
            {"data": data_uri(args.video)},
            "Describe the color sequence in chronological order and include any ordinal text you see. Be concise.",
        ),
    ]

    results = []
    for category, media_type, media_value, prompt in cases:
        payload = {
            "model": args.model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": media_type, media_type: media_value},
                        {"type": "text", "text": prompt},
                    ],
                }
            ],
            "temperature": 0,
            "seed": 42,
            "max_tokens": args.max_tokens,
            "stream": False,
        }
        body, client_wall_ms = request_chat(args.base_url, payload)
        message = body["choices"][0]["message"]
        results.append(
            {
                "category": category,
                "prompt": prompt,
                "reasoning": message.get("reasoning_content", ""),
                "output": message.get("content", ""),
                "finish_reason": body["choices"][0].get("finish_reason"),
                "usage": body.get("usage", {}),
                "timings": body.get("timings", {}),
                "client_wall_ms": client_wall_ms,
            }
        )

    report = {
        "schema": 1,
        "model": args.model,
        "image": str(args.image),
        "video": str(args.video),
        "results": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

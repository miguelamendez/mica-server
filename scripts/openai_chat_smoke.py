#!/usr/bin/env python3
"""Run a deterministic cross-task smoke suite against an OpenAI chat route."""

from __future__ import annotations

import argparse
import json
import time
import urllib.request
from pathlib import Path


PROMPTS = {
    "reasoning": "A farmer has 17 sheep. All but 9 run away. How many remain? Explain briefly.",
    "coding": "Write a Python function is_palindrome(s) that ignores case and non-alphanumeric characters. Return only one fenced code block.",
    "knowledge": "In four sentences, explain why seasons on Earth are caused primarily by axial tilt rather than distance from the Sun.",
    "creative": "Write a vivid 80-word scene about a lighthouse that receives a radio signal from the future.",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-tokens", type=int, default=256)
    args = parser.parse_args()

    results = []
    for category, prompt in PROMPTS.items():
        payload = {
            "model": args.model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0,
            "seed": 42,
            "max_tokens": args.max_tokens,
            "stream": False,
        }
        request = urllib.request.Request(
            args.base_url.rstrip("/") + "/v1/chat/completions",
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        start = time.perf_counter()
        with urllib.request.urlopen(request, timeout=600) as response:
            body = json.load(response)
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
                "client_wall_ms": (time.perf_counter() - start) * 1000,
            }
        )

    report = {"schema": 1, "model": args.model, "results": results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

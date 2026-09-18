#!/usr/bin/env python3
"""Benchmark OpenAI-compatible chat models across tasks, contexts, and concurrency.

This is an optional validation client, not a mica-server runtime dependency. It
uses only the Python standard library and records the actual prompt/completion
token counts reported by the selected backend.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import json
import math
import mimetypes
import statistics
import time
import urllib.request
from pathlib import Path


TASKS = {
    "reasoning": (
        "Use the context only as scratch material. A farmer has 17 sheep; all "
        "but 9 run away. Explain briefly how many remain."
    ),
    "coding": (
        "Write a Python function is_palindrome(s) that ignores case and "
        "non-alphanumeric characters. Return one fenced code block."
    ),
    "knowledge": (
        "In exactly four sentences, explain why Earth's seasons are caused "
        "primarily by axial tilt rather than distance from the Sun."
    ),
    "creativity": (
        "Write a vivid 120-word scene about a lighthouse receiving a radio "
        "signal from the future."
    ),
    "summary": (
        "Summarize the supplied synthetic project log in five concise bullets, "
        "preserving the first milestone, final milestone, and all risks."
    ),
}


def csv_values(value: str, converter=str) -> list:
    return [converter(item.strip()) for item in value.split(",") if item.strip()]


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def data_uri(path: Path) -> str:
    media_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    return f"data:{media_type};base64,{base64.b64encode(path.read_bytes()).decode()}"


def synthetic_context(target_tokens: int) -> str:
    # This deliberately remains tokenizer-independent. The server-reported
    # actual token count is authoritative and is stored beside the target.
    target_words = max(0, target_tokens - 96)
    anchors = (
        "The first milestone is alpha. The final milestone is omega. "
        "The open risks are memory, latency, and quality. "
    )
    # `record` is a stable single-token word in the target tokenizer families;
    # this avoids the severe overcount produced by unique hyphenated numbers.
    return anchors + "record " * target_words


def make_content(task: str, target_tokens: int, image: Path | None,
                 video: Path | None) -> str | list[dict]:
    if task == "image":
        if image is None:
            raise ValueError("the image task requires --image")
        return [
            {"type": "image_url", "image_url": {"url": data_uri(image)}},
            {"type": "text", "text": "Read all large text and describe the colored regions."},
        ]
    if task == "video":
        if video is None:
            raise ValueError("the video task requires --video")
        return [
            {"type": "input_video", "input_video": {"data": data_uri(video)}},
            {"type": "text", "text": "Describe the chronological sequence and ordinal text."},
        ]
    instruction = TASKS[task]
    return f"Context:\n{synthetic_context(target_tokens)}\n\nTask:\n{instruction}"


def send_chat(base_url: str, api_key: str, payload: dict, timeout: int) -> dict:
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    request = urllib.request.Request(
        base_url.rstrip("/") + "/v1/chat/completions",
        data=json.dumps(payload).encode(), headers=headers, method="POST",
    )
    start = time.perf_counter()
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = json.load(response)
    wall_seconds = time.perf_counter() - start
    choice = body["choices"][0]
    message = choice.get("message", {})
    return {
        "wall_seconds": wall_seconds,
        "finish_reason": choice.get("finish_reason"),
        "usage": body.get("usage", {}),
        "timings": body.get("timings", {}),
        "reasoning": message.get("reasoning_content", ""),
        "output": message.get("content", choice.get("text", "")),
    }


def summarize_run(results: list[dict], batch_wall_seconds: float) -> dict:
    latencies = [result["wall_seconds"] for result in results]
    prompt_tokens = [result["usage"].get("prompt_tokens", 0) for result in results]
    completion_tokens = [result["usage"].get("completion_tokens", 0) for result in results]
    backend_decode = [
        result["timings"].get("predicted_per_second", 0.0) for result in results
        if result["timings"].get("predicted_per_second") is not None
    ]
    return {
        "request_count": len(results),
        "batch_wall_seconds": batch_wall_seconds,
        "latency_seconds": {
            "min": min(latencies),
            "median": statistics.median(latencies),
            "p95": percentile(latencies, 0.95),
            "max": max(latencies),
        },
        "actual_prompt_tokens": {
            "min": min(prompt_tokens), "max": max(prompt_tokens),
            "mean": statistics.fmean(prompt_tokens),
        },
        "completion_tokens": sum(completion_tokens),
        "aggregate_completion_tokens_per_second": (
            sum(completion_tokens) / batch_wall_seconds if batch_wall_seconds else 0.0
        ),
        "mean_backend_decode_tokens_per_second": (
            statistics.fmean(backend_decode) if backend_decode else 0.0
        ),
        "all_stopped_normally": all(
            result["finish_reason"] == "stop" for result in results
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--api-key", default="")
    parser.add_argument("--api-key-file", type=Path)
    parser.add_argument("--model", required=True)
    parser.add_argument("--backend", required=True, choices=("mlx", "gguf", "vllm"))
    parser.add_argument("--quant", required=True)
    parser.add_argument("--tasks", default="reasoning,coding,knowledge,creativity,summary")
    parser.add_argument("--context-sizes", default="512,1024,2048,4096,8192,16384")
    parser.add_argument("--concurrency", default="1,2,4")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--max-output-tokens", type=int, default=256)
    parser.add_argument(
        "--peak-rss-gib", type=float,
        help="Measured peak/steady worker RSS to associate with this run",
    )
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--hardware-profile", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--video", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.api_key_file:
        args.api_key = args.api_key_file.read_text().strip()

    tasks = csv_values(args.tasks)
    unknown = set(tasks) - set(TASKS) - {"image", "video"}
    if unknown:
        parser.error("unknown tasks: " + ", ".join(sorted(unknown)))
    context_sizes = csv_values(args.context_sizes, int)
    concurrency_levels = csv_values(args.concurrency, int)
    if args.runs < 1 or any(value < 1 for value in concurrency_levels):
        parser.error("runs and concurrency must be positive")

    hardware = None
    if args.hardware_profile:
        hardware = json.loads(args.hardware_profile.read_text())

    cases = []
    for task in tasks:
        task_contexts = [0] if task in {"image", "video"} else context_sizes
        for target_context in task_contexts:
            content = make_content(task, target_context, args.image, args.video)
            for concurrency in concurrency_levels:
                payloads = []
                for run in range(args.runs):
                    for lane in range(concurrency):
                        payloads.append(
                            {
                                "model": args.model,
                                "messages": [{"role": "user", "content": content}],
                                "temperature": 0,
                                "seed": 42 + run * concurrency + lane,
                                "max_tokens": args.max_output_tokens,
                                "stream": False,
                            }
                        )
                started = time.perf_counter()
                with concurrent.futures.ThreadPoolExecutor(
                    max_workers=concurrency
                ) as executor:
                    futures = [
                        executor.submit(
                            send_chat, args.base_url, args.api_key, payload, args.timeout
                        )
                        for payload in payloads
                    ]
                    results = [future.result() for future in futures]
                batch_wall = time.perf_counter() - started
                cases.append(
                    {
                        "task": task,
                        "target_context_tokens": target_context,
                        "concurrency": concurrency,
                        "runs": args.runs,
                        "summary": summarize_run(results, batch_wall),
                        "requests": results,
                    }
                )

    report = {
        "schema": 1,
        "created_unix_seconds": time.time(),
        "model": args.model,
        "backend": args.backend,
        "quantization": args.quant,
        "base_url": args.base_url,
        "max_output_tokens": args.max_output_tokens,
        "peak_rss_gib": args.peak_rss_gib,
        "hardware": hardware,
        "cases": cases,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run Mica's small/medium/long profiles sequentially for each backend."""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TIERS = {
    "small": {"context": 512, "output": 128, "concurrency": 4},
    "medium": {"context": 4096, "output": 256, "concurrency": 2},
    "long": {"context": 16384, "output": 512, "concurrency": 1},
}
MODELS = {
    "mlx": "spark-x25-4b",
    "gguf": "spark-x25-4b",
    "vllm": "vllm-qwen3-06b-control",
}


def wait_ready(base_url: str, process: subprocess.Popen, timeout: int) -> dict:
    deadline = time.monotonic() + timeout
    last = "not started"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with {process.returncode}")
        try:
            with urllib.request.urlopen(base_url + "/ready", timeout=2) as response:
                payload = json.load(response)
                if response.status == 200 and payload.get("ready"):
                    return payload
        except urllib.error.HTTPError as error:
            last = error.read().decode(errors="replace")
        except Exception as error:
            last = str(error)
        time.sleep(0.5)
    raise TimeoutError(f"server readiness timed out: {last}")


def run_case(args: argparse.Namespace, backend: str, tier: str) -> dict:
    profile = f"{backend}-{tier}"
    definition = TIERS[tier]
    case_dir = args.output_dir / backend / tier
    case_dir.mkdir(parents=True, exist_ok=True)
    setup_log = case_dir / "setup.log"
    server_log = case_dir / "server.log"
    benchmark = case_dir / "benchmark.json"
    setup = [str(args.binary), "setup", "--backend", backend, "--profile", profile,
             "--quant", "q4", "--ram-gib", str(args.ram_gib), "--root", str(args.model_root),
             "--hardware-profile", str(args.hardware_profile)]
    started = time.perf_counter()
    with setup_log.open("w") as output:
        subprocess.run(setup, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True)
    log = server_log.open("w")
    process = subprocess.Popen(
        [str(args.binary), "serve", "--backend", backend, "--root", str(args.model_root),
         "--port", str(args.port)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
    )
    try:
        wait_ready(f"http://127.0.0.1:{args.port}", process, args.ready_timeout)
        command = [
            sys.executable, str(ROOT / "scripts/benchmark_matrix.py"),
            "--base-url", f"http://127.0.0.1:{args.port}",
            "--api-key-file", str(args.model_root / "mica-server/api-key"),
            "--model", MODELS[backend], "--backend", backend, "--quant", "q4",
            "--tasks", "summary", "--context-sizes", str(definition["context"]),
            "--concurrency", str(definition["concurrency"]), "--runs", "1",
            "--max-output-tokens", str(definition["output"]),
            "--timeout", str(args.request_timeout), "--output", str(benchmark),
        ]
        subprocess.run(command, cwd=ROOT, stdout=subprocess.DEVNULL, check=True)
        payload = json.loads(benchmark.read_text())
        summary = payload["cases"][0]["summary"]
        return {
            "backend": backend, "tier": tier, "profile": profile,
            "model": MODELS[backend], "passed": True,
            "target_input_tokens": definition["context"],
            "actual_prompt_tokens": summary["actual_prompt_tokens"],
            "completion_tokens": summary["completion_tokens"],
            "concurrency": definition["concurrency"],
            "batch_wall_seconds": summary["batch_wall_seconds"],
            "aggregate_completion_tokens_per_second":
                summary["aggregate_completion_tokens_per_second"],
            "elapsed_seconds": time.perf_counter() - started,
        }
    finally:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        log.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/mica-server")
    parser.add_argument("--model-root", type=Path, default=Path.home() / "models")
    parser.add_argument("--hardware-profile", type=Path,
                        default=ROOT / "artifacts/hardware-profile.json")
    parser.add_argument("--backends", default="mlx,gguf,vllm")
    parser.add_argument("--tiers", default="small,medium,long")
    parser.add_argument("--ram-gib", type=float, default=8.0)
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--ready-timeout", type=int, default=600)
    parser.add_argument("--request-timeout", type=int, default=1800)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "artifacts/benchmarks/profile-acceptance")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "artifacts/benchmarks/profile-acceptance.json")
    args = parser.parse_args()
    backends = [item for item in args.backends.split(",") if item]
    tiers = [item for item in args.tiers.split(",") if item]
    if set(backends) - set(MODELS) or set(tiers) - set(TIERS):
        parser.error("unknown backend or tier")
    results = []
    for backend in backends:
        for tier in tiers:
            print(f"running {backend}/{tier}", flush=True)
            try:
                result = run_case(args, backend, tier)
            except Exception as error:
                result = {"backend": backend, "tier": tier,
                          "profile": f"{backend}-{tier}", "passed": False,
                          "error": str(error)}
            results.append(result)
            print(json.dumps(result), flush=True)
    report = {"schema": 1,
              "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "ram_limit_gib": args.ram_gib,
              "passed": all(item["passed"] for item in results), "cases": results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

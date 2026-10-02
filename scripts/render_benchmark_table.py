#!/usr/bin/env python3
"""Render benchmark-matrix JSON reports as a Markdown hardware table."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


START = "<!-- MICA_BENCHMARK_TABLE_START -->"
END = "<!-- MICA_BENCHMARK_TABLE_END -->"


def hardware_name(report: dict) -> str:
    profile = report.get("hardware") or {}
    cpu = profile.get("cpu", {})
    system = profile.get("system", {})
    return cpu.get("model") or f"{system.get('os', 'unknown')} {system.get('arch', '')}".strip()


def render(reports: list[dict]) -> str:
    rows = []
    for report in reports:
        for case in report.get("cases", []):
            summary = case["summary"]
            actual = summary["actual_prompt_tokens"]
            latency = summary["latency_seconds"]
            rows.append(
                [
                    hardware_name(report),
                    report["model"],
                    report["backend"],
                    report["quantization"],
                    case["task"],
                    str(case["target_context_tokens"] or "media"),
                    f"{actual['mean']:.0f}",
                    str(case["concurrency"]),
                    f"{summary['mean_backend_decode_tokens_per_second']:.2f}",
                    f"{summary['aggregate_completion_tokens_per_second']:.2f}",
                    f"{latency['median']:.3f}",
                    f"{latency['p95']:.3f}",
                    (f"{report['peak_rss_gib']:.2f}" if report.get("peak_rss_gib") else "—"),
                ]
            )
    rows.sort(key=lambda row: (row[0], row[1], row[2], row[3], row[4], row[5], row[7]))
    header = [
        "Hardware", "Model", "Backend", "Quant", "Task", "Target input",
        "Actual input", "Concurrency", "Mean decode tok/s", "Aggregate tok/s",
        "Median latency s", "p95 latency s", "Worker RSS GiB",
    ]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--readme", type=Path)
    args = parser.parse_args()
    table = render([json.loads(path.read_text()) for path in args.reports])
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(table + "\n")
    if args.readme:
        text = args.readme.read_text()
        if START not in text or END not in text:
            raise SystemExit(f"README must contain {START} and {END}")
        before, remainder = text.split(START, 1)
        _, after = remainder.split(END, 1)
        args.readme.write_text(before + START + "\n" + table + "\n" + END + after)
    if not args.output and not args.readme:
        print(table)


if __name__ == "__main__":
    main()

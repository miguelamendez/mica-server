#!/usr/bin/env python3
"""Run a conversion command and stop its process tree above an RSS limit."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time


def process_table() -> dict[int, tuple[int, int]]:
    result = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss="],
        check=True,
        capture_output=True,
        text=True,
    )
    table: dict[int, tuple[int, int]] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:
            table[int(fields[0])] = (int(fields[1]), int(fields[2]))
    return table


def tree_rss_kib(root_pid: int) -> int:
    table = process_table()
    descendants = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent, _) in table.items():
            if parent in descendants and pid not in descendants:
                descendants.add(pid)
                changed = True
    return sum(table.get(pid, (0, 0))[1] for pid in descendants)


def terminate_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit-gib", type=float, required=True)
    parser.add_argument("--poll-seconds", type=float, default=0.25)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if args.limit_gib <= 0 or args.poll_seconds <= 0 or not command:
        parser.error("a positive limit, poll interval, and command are required")

    limit_kib = int(args.limit_gib * 1024 * 1024)
    child = subprocess.Popen(command, start_new_session=True)
    peak_kib = 0

    def forward(signum: int, _frame: object) -> None:
        if child.poll() is None:
            os.killpg(child.pid, signum)

    signal.signal(signal.SIGINT, forward)
    signal.signal(signal.SIGTERM, forward)

    exceeded = False
    monitor_failures = 0
    while child.poll() is None:
        try:
            rss_kib = tree_rss_kib(child.pid)
            monitor_failures = 0
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            monitor_failures += 1
            if monitor_failures >= 3:
                print(
                    f"memory monitor unavailable after {monitor_failures} attempts: "
                    f"{error}; terminating process tree",
                    file=sys.stderr,
                    flush=True,
                )
                terminate_group(child)
                return 76
            time.sleep(args.poll_seconds)
            continue
        peak_kib = max(peak_kib, rss_kib)
        if rss_kib > limit_kib:
            exceeded = True
            print(
                f"memory limit exceeded: {rss_kib / 1024 / 1024:.2f} GiB "
                f"> {args.limit_gib:.2f} GiB; terminating process tree",
                file=sys.stderr,
                flush=True,
            )
            terminate_group(child)
            break
        time.sleep(args.poll_seconds)

    return_code = child.wait()
    print(
        f"peak process-tree RSS: {peak_kib / 1024 / 1024:.2f} GiB "
        f"(limit {args.limit_gib:.2f} GiB)",
        file=sys.stderr,
        flush=True,
    )
    return 75 if exceeded else return_code


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Real inference and overlap-aware workload swaps; optional stdlib test client.

Install tests/fixtures/coder-qwen-swap.yaml before starting the server.
Uses a private key file and checkpoints raw outputs without storing the key.
"""
from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import time
import urllib.error
import urllib.request
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:8092")
    parser.add_argument("--api-key-file", type=Path, required=True)
    parser.add_argument("--profile", default="mica-coder-qwen-gguf")
    parser.add_argument("--swap-profile", default="coder-qwen-swap-test")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    key = args.api_key_file.read_text().strip()
    report = {"schema": 1, "complete": False, "passed": False,
              "profile": args.profile, "checks": [], "cases": []}

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    def check(name, condition, detail=None):
        report["checks"].append({"name": name, "passed": bool(condition),
                                 "detail": detail})
        save()
        print(f"{'PASS' if condition else 'FAIL'} {name}", flush=True)
        if not condition:
            raise RuntimeError(name)

    def request(path, body=None, authenticated=True):
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["Authorization"] = "Bearer " + key
        req = urllib.request.Request(args.base_url.rstrip("/") + path,
            data=None if body is None else json.dumps(body).encode(), headers=headers)
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=args.timeout) as response:
                result = {"status": response.status, "body": json.load(response)}
        except urllib.error.HTTPError as error:
            result = {"status": error.code, "body": json.load(error)}
        result["wall_seconds"] = time.perf_counter() - started
        return result

    def ready():
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                if request("/ready", authenticated=False)["status"] == 200:
                    return
            except (urllib.error.URLError, TimeoutError):
                # systemd start returns before the server binds its socket.
                pass
            time.sleep(1)
        raise TimeoutError("server not ready")

    def state():
        result = request("/admin/models")
        check("admin state available", result["status"] == 200)
        body = result["body"]
        check("at most one resident worker", len(body["workers"]) <= 1)
        return body

    def activate(profile):
        result = request("/admin/profile/activate", {"profile": profile})
        report["cases"].append({"kind": "workload_swap", "result": result})
        check("activate " + profile, result["status"] == 200, result["body"])
        ready()
        return result["body"]

    def worker(body, model):
        return next(item for item in body["workers"]
                    if item["id"].split("@")[0] == model)

    def uri(path):
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        return "data:" + mime + ";base64," + base64.b64encode(path.read_bytes()).decode()

    def chat(name, content, model=None):
        body = {"messages": [{"role": "user", "content": content}],
                "max_tokens": 512, "temperature": 0, "stream": False,
                "chat_template_kwargs": {"enable_thinking": False}}
        if model:
            body["model"] = model
        result = request("/v1/chat/completions", body)
        report["cases"].append({"kind": name, "request": body, "result": result})
        check(name + " HTTP success", result["status"] == 200, result["body"])
        text = result["body"]["choices"][0]["message"].get("content", "") or ""
        check(name + " produced text", bool(text.strip()))
        check(name + " normal stop", result["body"]["choices"][0]["finish_reason"] == "stop")
        save()
        return text

    try:
        ready()
        unauth = request("/v1/models", authenticated=False)
        check("API requires authentication", unauth["status"] == 401)
        listing = request("/v1/models")
        report["models"] = listing
        check("active workload lists models", listing["status"] == 200)
        initial = state()
        check("correct initial workload", initial["profile"]["name"] == args.profile)
        text = chat("default_spark_text", "What is 17 times 19? Reply only with the integer.")
        check("Spark arithmetic", "323" in text, text)
        before = state()
        spark_pid = worker(before, "spark-x25-4b")["pid"]
        switched = activate(args.swap_profile)
        check("Spark retained in smaller workload", bool(switched.get("retained")))
        check("Spark PID unchanged", worker(state(), "spark-x25-4b")["pid"] == spark_pid)
        denied = request("/v1/chat/completions", {"model": "qwen38-27b-gsq-rco",
            "messages": [{"role": "user", "content": "Hi"}], "max_tokens": 8})
        check("excluded coder rejected", 400 <= denied["status"] < 500, denied)
        activate(args.profile)
        check("Spark retained on return", worker(state(), "spark-x25-4b")["pid"] == spark_pid)
        text = chat("qwen27_reasoning", "A farmer has 17 sheep. All but 9 run away. How many remain? Reply with the number and one short explanation.", "qwen38-27b-gsq-rco")
        check("Qwen27 reasoning answer", "9" in text or "nine" in text.lower(), text)
        before = state()
        excluded_id = worker(before, "qwen38-27b-gsq-rco")["id"]
        switched = activate(args.swap_profile)
        check("excluded resident coder unloaded", excluded_id in switched.get("unloaded", []), switched)
        check("excluded coder absent", not any("qwen38-27b" in item["id"] for item in state()["workers"]))
        activate(args.profile)
        text = chat("default_qwen9_image", [
            {"type": "image_url", "image_url": {"url": uri(args.image)}},
            {"type": "text", "text": "Read the large text and name the colors of the upper-left and lower-right squares."}])
        check("image OCR and colors", all(word in text.lower() for word in ("31415", "blue", "red")), text)
        before = state()
        vision_pid = worker(before, "qwen35-9b")["pid"]
        switched = activate(args.swap_profile)
        check("vision retained in smaller workload", bool(switched.get("retained")))
        check("vision PID unchanged", worker(state(), "qwen35-9b")["pid"] == vision_pid)
        activate(args.profile)
        text = chat("default_qwen9_video", [
            {"type": "input_video", "input_video": {"data": uri(args.video)}},
            {"type": "text", "text": "List the three background colors in chronological order, from the beginning to the end."}])
        lowered = text.lower()
        check("video temporal colors", all(word in lowered for word in ("red", "green", "blue")) and
              lowered.index("red") < lowered.index("green") < lowered.index("blue"), text)
        check("video reused vision worker", worker(state(), "qwen35-9b")["pid"] == vision_pid)
        stream_body = {"messages": [{"role": "user", "content": "What is 2 plus 2? Reply only with the integer."}],
                       "stream": True, "max_tokens": 32, "temperature": 0,
                       "chat_template_kwargs": {"enable_thinking": False}}
        req = urllib.request.Request(args.base_url.rstrip("/") + "/v1/chat/completions",
            data=json.dumps(stream_body).encode(), headers={"Authorization": "Bearer " + key,
                                                            "Content-Type": "application/json"})
        events, chunks, done = [], [], False
        started = time.perf_counter()
        with urllib.request.urlopen(req, timeout=args.timeout) as response:
            for raw in response:
                line = raw.decode().strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    done = True
                    break
                event = json.loads(data)
                events.append(event)
                for choice in event.get("choices", []):
                    chunks.append(choice.get("delta", {}).get("content", "") or "")
        report["cases"].append({"kind": "stream", "events": events, "done": done,
                                "wall_seconds": time.perf_counter() - started})
        check("stream completed with answer", done and "4" in "".join(chunks))
        report["final_state"] = state()
        check("original workload restored", report["final_state"]["profile"]["name"] == args.profile)
        report["complete"] = report["passed"] = True
        save()
        return 0
    except Exception as error:
        report["error"] = str(error)
        save()
        print("FAILED: " + str(error), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

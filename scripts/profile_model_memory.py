#!/usr/bin/env python3
"""Offline, sequential memory measurements. No new Mica serving dependency.

Plan by default; --execute launches installed engines using cached artifacts.
Capacity-fill is teacher-forced prefill, NOT a claim about autoregressive peak.
Results belong to a model/artifact/engine/hardware tuple, never a workload.
"""
from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import importlib.metadata
import json
import os
import platform
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

GIB = 1024 ** 3
KV_TYPES = {"q4": "q4_0", "q8": "q8_0", "f16": "f16"}


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def with_gib(value):
    """Keep authoritative bytes and add convenient binary GiB display fields."""
    if isinstance(value, list):
        return [with_gib(item) for item in value]
    if not isinstance(value, dict):
        return value
    result = {key: with_gib(item) for key, item in value.items()}
    for key, item in value.items():
        if key.endswith("_bytes") and isinstance(item, (int, float)) and not isinstance(item, bool):
            result[key[:-6] + "_gib"] = round(item / GIB, 6)
    return result


def load_manifest(path):
    import yaml  # Offline dependency only; --mlx-worker does not need it.
    document = yaml.safe_load(Path(path).read_text())
    if document.get("schema") != 2:
        raise ValueError("expected a schema-2 model manifest")
    return document


def contained(base, relative):
    base = Path(base).resolve()
    path = (base / relative).resolve()
    if not path.is_relative_to(base):
        raise ValueError("artifact path escapes its model cache")
    return path


def select_artifact(model, artifact_id, engine=None):
    matches = [a for a in model["artifacts"] if a["id"] == artifact_id
               and (engine is None or engine in a["compatible_engines"])]
    if not matches:
        raise ValueError("no matching model artifact/engine")
    artifact = matches[0]
    return artifact, engine or artifact["compatible_engines"][0]


def token_budget(model, input_tokens, output_tokens):
    maximum = model.get("native_context_tokens")
    if type(maximum) is not int or maximum <= 0:
        raise ValueError("model needs a native context limit")
    recommended = model.get("recommended_context_tokens")
    supported_output = model.get("max_output_tokens")
    for limit in (recommended, supported_output):
        if limit is not None and (type(limit) is not int or not 0 < limit <= maximum):
            raise ValueError("recommended context and maximum output must fit native context")
    default_total = recommended or maximum
    explicit_output = output_tokens is not None
    if output_tokens is None:
        if supported_output is None:
            raise ValueError("output limit unverified: specify --output-tokens explicitly")
        output_tokens = supported_output
    if input_tokens is None:
        input_tokens = default_total - output_tokens
    if (type(input_tokens) is not int or type(output_tokens) is not int or
            input_tokens < 1 or output_tokens < 1 or input_tokens + output_tokens > maximum):
        raise ValueError("input + output must fit the native total context")
    if supported_output and output_tokens > supported_output:
        raise ValueError("output budget exceeds the model's verified output limit")
    return {"input_tokens": input_tokens, "output_reserve_tokens": output_tokens,
            "total_tokens": input_tokens + output_tokens,
            "native_total_context_tokens": maximum,
            "recommended_total_context_tokens": recommended,
            "recommended_context_exceeded": bool(recommended and input_tokens + output_tokens > recommended),
            "output_limit_source": "cli-override" if explicit_output else "model"}


def file_inventory(model, artifact, cache_root):
    base = Path(cache_root) / "models" / artifact["format"] / model["id"]
    inventory = []
    for entry in artifact["files"]:
        path = contained(base, entry["path"])
        size = (sum(p.stat().st_size for p in path.rglob("*") if p.is_file())
                if path.is_dir() else path.stat().st_size if path.is_file() else None)
        inventory.append({"role": entry["role"], "path": str(path),
                          "cache_relative_path": str(path.relative_to(Path(cache_root).resolve())),
                          "declared_bytes": entry["size_bytes"], "disk_bytes": size,
                          "sha256": entry.get("sha256"), "source": entry.get("source")})
    return inventory


def rss_bytes(pid):
    data = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True, timeout=3)
    return int(data.strip()) * 1024


def footprint_bytes(pid):
    """macOS physical footprint includes charged unified-memory allocations.

    It is kept separate from RSS/Metal bytes; these metrics overlap, not add up.
    """
    if sys.platform != "darwin":
        return None
    class Usage(ctypes.Structure):
        _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [
            (name, ctypes.c_uint64) for name in (
                "user", "system", "idle", "interrupt", "pageins", "wired",
                "resident", "footprint", "started", "exited")]
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    library.proc_pid_rusage.restype = ctypes.c_int
    usage = Usage()
    if library.proc_pid_rusage(pid, 0, ctypes.byref(usage)) != 0:
        raise OSError(ctypes.get_errno(), "proc_pid_rusage failed")
    return usage.footprint


def pressure():
    if sys.platform == "darwin":
        return int(subprocess.check_output(
            ["sysctl", "-n", "kern.memorystatus_vm_pressure_level"], text=True, timeout=3))
    return None


def available_bytes():
    if sys.platform == "linux":
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    return None  # macOS pressure is more useful than "free" pages.


def gpu_bytes(pid):
    """NVIDIA process VRAM, not device-wide usage; other GPU metrics unavailable."""
    if sys.platform != "linux":
        return None
    try:
        data = subprocess.check_output([
            "nvidia-smi", "--query-compute-apps=pid,used_gpu_memory",
            "--format=csv,noheader,nounits"], text=True, timeout=3)
    except FileNotFoundError:
        return None
    return sum(int(line.split(",")[1].strip()) * 1024**2
               for line in data.splitlines() if line.split(",")[0].strip() == str(pid))


def memory_snapshot(pid):
    return {"rss_bytes": rss_bytes(pid), "physical_footprint_bytes": footprint_bytes(pid),
            "nvidia_vram_bytes": gpu_bytes(pid)}


def stop_process(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


class Watchdog:
    def __init__(self, process, limit_gib, timeout):
        self.process, self.limit, self.timeout = process, int(limit_gib * GIB), timeout
        self.done = threading.Event()
        self.error = None
        self.peaks = {}
        self.thread = threading.Thread(target=self.watch, daemon=True)

    def watch(self):
        started = time.monotonic()
        try:
            while not self.done.wait(.25) and self.process.poll() is None:
                sample = memory_snapshot(self.process.pid)
                for key, value in sample.items():
                    if value is not None:
                        self.peaks[key] = max(value, self.peaks.get(key, 0))
                if any(v is not None and v > self.limit for v in sample.values()):
                    raise MemoryError("process memory safeguard exceeded")
                if pressure() in (2, 4):
                    raise MemoryError("macOS system memory pressure")
                free = available_bytes()
                if free is not None and free < GIB:
                    raise MemoryError("less than 1 GiB available system RAM")
                if time.monotonic() - started > self.timeout:
                    raise TimeoutError("measurement deadline")
        except Exception as error:
            # Fail closed on monitoring errors, but don't call a natural exit a failure.
            if self.process.poll() is None:
                self.error = str(error)
                stop_process(self.process)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.done.set()
        self.thread.join(timeout=5)
        stop_process(self.process)


def request(url, path, body=None, timeout=3600):
    req = urllib.request.Request(url + path,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def parse_buffers(text):
    # Preserve engine log measurements and labels; do not infer resident memory
    # from file sizes. KV and recurrent buffers can coexist in hybrid models.
    result = []
    for line in text.splitlines():
        match = re.search(r"(.+?)(?:buffer size|compute buffer size)\s*=\s*([\d.]+)\s*(MiB|GiB)", line)
        if match:
            result.append({"label": match[1].strip(),
                           "allocated_bytes": round(float(match[2]) * (1024**2 if match[3] == "MiB" else GIB))})
    return result


def reported_kv_types(text):
    return [{"key": key.lower(), "value": value.lower()}
            for key, value in re.findall(r"K \(([^)]+)\):.*?V \(([^)]+)\):", text)]


def llama_command(binary, model_path, kv_type, budget, port, args, projector=None):
    cmd = [str(binary), "-m", str(model_path), "--host", "127.0.0.1", "--port", str(port),
           "-c", str(budget["total_tokens"]), "-np", "1", "-ngl", str(args.gpu_layers),
           "-fa", "on", "-ctk", KV_TYPES[kv_type], "-ctv", KV_TYPES[kv_type],
           "--fit", "off", "--batch-size", str(args.batch_size),
           "--ubatch-size", str(args.prefill_chunk), "--cache-ram", "0", "--ctx-checkpoints", "0",
           "--threads", str(args.threads), "--threads-batch", str(args.threads), "--no-webui"]
    if projector:
        cmd += ["--mmproj", str(projector), "--image-min-tokens", str(args.image_tokens),
                "--image-max-tokens", str(args.image_tokens)]
    return cmd


def fixture_image(size=768):
    """Deterministic RGB fixture, no imaging dependency or private user media."""
    row = b"\x00" + b"\x68\xa0\xcc" * size
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(row * size)) + chunk(b"IEND", b""))


def run_llama(binary, path, kv_type, budget, args, directory, projector=None):
    port = unused_port()
    url = f"http://127.0.0.1:{port}"
    result = {"status": "failed", "snapshots": [], "engine_buffers": [],
              "measurement_kind": "teacher-forced-capacity-fill",
              "projector_attached": projector is not None,
              "autoregressive_max_output_measured": False}
    log_path = directory / ("projector.log" if projector else "main.log")
    cmd = llama_command(binary, path, kv_type, budget, port, args, projector)
    # Store portable launch parameters, not a person's absolute home paths.
    result["launch"] = {"context_tokens": budget["total_tokens"], "slots": 1,
        "gpu_layers": args.gpu_layers, "flash_attention": True,
        "key_type": KV_TYPES[kv_type], "value_type": KV_TYPES[kv_type],
        "batch_size": args.batch_size, "microbatch_size": args.prefill_chunk,
        "context_checkpoints": 0, "ram_prompt_cache_mib": 0}
    started = time.monotonic()
    with log_path.open("w") as log:
        process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        with Watchdog(process, args.limit_gib, args.timeout) as watchdog:
            try:
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("worker exited loading; see log")
                    if time.monotonic() - started > min(180, args.timeout):
                        raise TimeoutError("worker load deadline")
                    try:
                        request(url, "/health", timeout=1)
                        break
                    except (urllib.error.URLError, TimeoutError):
                        time.sleep(.25)
                result["snapshots"].append({"phase": "loaded", **memory_snapshot(process.pid)})
                unit = request(url, "/tokenize", {"content":
                    "Memory profiling reference. Read the records and return a brief summary.\n"})["tokens"]
                if not unit:
                    raise RuntimeError("tokenizer returned an empty sequence")
                # Native token IDs give exact occupation. No BOS/chat overhead estimates.
                for phase, count in (("input_prefilled", budget["input_tokens"]),
                                     ("capacity_filled", budget["total_tokens"] - 1)):
                    tokens = (unit * (count // len(unit) + 1))[:count]
                    response = request(url, "/completion", {
                        "prompt": tokens, "n_predict": 1, "ignore_eos": True,
                        "cache_prompt": True, "temperature": 0, "n_ctx": budget["total_tokens"]})
                    actual = response.get("tokens_evaluated", response.get("timings", {}).get("prompt_n"))
                    # tokens_evaluated includes cached prefix; prompt_n may not.
                    if actual != count:
                        raise RuntimeError(f"unverified prompt occupation: requested {count}, reported {actual}")
                    result["snapshots"].append({"phase": phase, "occupied_prompt_tokens": actual,
                        "generated_tokens": response.get("tokens_predicted"),
                        "timings": response.get("timings"), **memory_snapshot(process.pid)})
                if args.decode_tokens:
                    count = min(args.decode_tokens, budget["output_reserve_tokens"])
                    if count:
                        # This separate short decode is not a maximum-output peak measurement.
                        tokens = (unit * (budget["input_tokens"] // len(unit) + 1))[:budget["input_tokens"]]
                        response = request(url, "/completion", {"prompt": tokens,
                            "n_predict": count, "ignore_eos": True, "cache_prompt": True, "temperature": 0})
                        if response.get("tokens_predicted") != count:
                            raise RuntimeError("short decode did not reach its requested token count")
                        result["snapshots"].append({"phase": "short_decode", "generated_tokens": count,
                            "timings": response.get("timings"), **memory_snapshot(process.pid)})
                if projector:
                    image = base64.b64encode(fixture_image()).decode()
                    response = request(url, "/v1/chat/completions", {
                        "messages": [{"role": "user", "content": [
                            {"type": "image_url", "image_url": {"url": "data:image/png;base64," + image}},
                            {"type": "text", "text": "Describe the image briefly."}]}],
                        "max_tokens": 8, "temperature": 0,
                        "chat_template_kwargs": {"enable_thinking": False}})
                    result["snapshots"].append({"phase": "image_processed", "image_size": [768, 768],
                        "image_token_limit": args.image_tokens,
                        "usage": response.get("usage"), **memory_snapshot(process.pid)})
                    result["image_processing_measured"] = True
                    result["image_at_full_context_measured"] = False
                result["status"] = "measured"
            except Exception as error:
                result["error"] = str(error)
        result["observed_peaks"] = watchdog.peaks
        if watchdog.error:
            result.update(status="guard_stopped", error=watchdog.error)
    text = log_path.read_text(errors="replace")
    result["engine_buffers"] = parse_buffers(text)
    result["kv_types_reported"] = reported_kv_types(text)
    result["engine_version"] = text.splitlines()[:8]
    result["elapsed_seconds"] = time.monotonic() - started
    # Failure details remain in the log; a q4/q8 fallback must never count as success.
    if result["status"] == "measured" and re.search(r"unsupported.*(?:cache|q[48])|cache.*not supported", text, re.I):
        result.update(status="unsupported", error="engine rejected requested cache precision")
    if result["status"] == "measured" and (not result["kv_types_reported"] or any(
            row["key"] != KV_TYPES[kv_type] or row["value"] != KV_TYPES[kv_type]
            for row in result["kv_types_reported"])):
        result.update(status="unverified", error="could not verify requested K/V precision in engine logs")
    return result


def mlx_worker(spec_path, result_path):
    spec = json.loads(Path(spec_path).read_text())
    result = {"status": "failed", "measurement_kind": "teacher-forced-capacity-fill",
              "autoregressive_max_output_measured": False, "snapshots": []}
    try:
        import mlx.core as mx
        from mlx.utils import tree_flatten
        from mlx_worker import configure_memory
        from mlx_vlm import load
        from mlx_vlm.generate.common import maybe_quantize_kv_cache
        configure_memory(mx, spec["limit_gib"])
        result["versions"] = {name: importlib.metadata.version(name) for name in ("mlx", "mlx-vlm")}
        model, processor = load(spec["path"])
        mx.eval(model.parameters())
        weights = sum(a.nbytes for _, a in tree_flatten(model.parameters()))
        language = model.language_model
        bits = {"q4": 4, "q8": 8, "f16": None}[spec["kv_type"]]
        result["cache_path"] = spec["mlx_cache_path"]
        if spec["mlx_cache_path"] == "batch":
            # Use the actual server's cache builder, including its rotating and
            # protected-layer behavior, with one sequence and no left padding.
            from mlx_vlm.generate.ar import _make_cache
            cache = _make_cache(language, [0], kv_bits=bits)
        else:
            cache = language.make_cache()
        tokenizer = getattr(processor, "tokenizer", processor)
        unit = tokenizer.encode("Memory profiling reference. Return a brief summary.\n", add_special_tokens=False)
        if not unit:
            raise RuntimeError("empty tokenizer output")
        def snapshot(phase, count):
            mx.synchronize()
            layouts = [{"layer": i, "type": type(c).__name__, "bits": getattr(c, "bits", None),
                        "allocated_bytes": getattr(c, "nbytes", None),
                        "offset": (getattr(c, "offset").tolist() if hasattr(getattr(c, "offset", None), "tolist")
                                   else getattr(c, "offset", None))}
                       for i, c in enumerate(cache)]
            result["snapshots"].append({"phase": phase, "occupied_prompt_tokens": count,
                "mlx_active_bytes": mx.get_active_memory(), "mlx_allocator_cache_bytes": mx.get_cache_memory(),
                "mlx_peak_bytes": mx.get_peak_memory(), "cache_layers": layouts,
                "kv_and_recurrent_bytes": sum(c["allocated_bytes"] or 0 for c in layouts),
                **memory_snapshot(os.getpid())})
            save(result_path, result)
        result["weight_tensor_bytes"] = weights
        snapshot("loaded", 0)
        processed = 0
        for phase, target in (("input_prefilled", spec["budget"]["input_tokens"]),
                              ("capacity_filled", spec["budget"]["total_tokens"])):
            while processed < target:
                count = min(spec["prefill_chunk"], target - processed)
                tokens = [unit[(processed + i) % len(unit)] for i in range(count)]
                output = language(mx.array([tokens]), cache=cache, logits_to_keep=1)
                mx.eval(output.logits[:, -1, :], [c.state for c in cache])
                # Same quantization policy as Mica's mlx-vlm launcher. It may
                # preserve sliding-window/last-layer caches; report every layer.
                if spec["mlx_cache_path"] == "stream":
                    maybe_quantize_kv_cache(cache, 0, 64, bits)
                mx.eval([c.state for c in cache])
                processed += count
                del output
                mx.clear_cache()
                if processed % 4096 == 0:
                    snapshot("prefill_progress", processed)
            snapshot(phase, processed)
        quantized = [c for c in cache if getattr(c, "bits", None) == bits] if bits else []
        if bits and not quantized:
            result.update(status="unsupported", error="requested quantization produced no quantized cache layers")
        else:
            result["status"] = "measured"
            result["cache_quantization_scope"] = "mixed" if bits and len(quantized) < len(cache) else "all-kv"
    except NotImplementedError as error:
        result.update(status="unsupported", error=str(error))
    except Exception as error:
        result["error"] = type(error).__name__ + ": " + str(error)
    save(result_path, result)
    return 0 if result["status"] == "measured" else 1


def run_mlx(path, kv_type, budget, args, directory):
    spec_path, result_path = directory / "worker-spec.json", directory / "main.json"
    save(spec_path, {"path": str(path), "kv_type": kv_type, "budget": budget,
                     "limit_gib": args.limit_gib, "prefill_chunk": args.prefill_chunk,
                     "mlx_cache_path": args.mlx_cache_path})
    with (directory / "main.log").open("w") as log:
        process = subprocess.Popen([str(args.mlx_python), str(Path(__file__).resolve()),
            "--mlx-worker", str(spec_path), str(result_path)], stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True)
        with Watchdog(process, args.limit_gib, args.timeout) as watchdog:
            process.wait()
        result = json.loads(result_path.read_text()) if result_path.exists() else {"status": "failed", "error": "no worker result"}
        result["observed_peaks"] = watchdog.peaks
        if watchdog.error:
            result.update(status="guard_stopped", error=watchdog.error)
        if process.returncode and result["status"] == "measured":
            result.update(status="failed", error="worker exited unsuccessfully")
    spec_path.unlink()  # ephemeral, contains machine-local paths; not an artifact.
    result["short_decode_measured"] = False
    return result


def hardware(args):
    document = {"os": platform.system(), "architecture": platform.machine(),
                "cpu_threads_available": os.cpu_count()}
    profile = Path(args.cache_root) / "state/hardware-profile.json"
    if profile.is_file():
        original = json.loads(profile.read_text())
        # Do not copy arbitrary local paths/credentials into a shareable report.
        document["detected"] = {key: original[key] for key in (
            "os", "arch", "cpu", "cpu_model", "cpu_vendor", "ram_gib", "unified_memory_gib",
            "logical_cores", "physical_cores", "accelerators", "apple_silicon",
            "system", "memory") if key in original}
    return document


def profile_one(manifest_path, args):
    model = load_manifest(manifest_path)
    artifact, engine = select_artifact(model, args.artifact, args.engine)
    budget = token_budget(model, args.input_tokens, args.output_tokens)
    inventory = file_inventory(model, artifact, args.cache_root)
    report = {"schema": 1, "model": model["id"], "artifact": artifact["id"],
        "weight_quantization": artifact["quantization_type"], "engine": engine,
        "repository": artifact["repository"], "revision": artifact["revision"],
        "hardware": hardware(args), "budget": budget,
        "memory_limit_gib": args.limit_gib, "concurrent_sequences": 1,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "files": [{k: v for k, v in f.items() if k != "path"} for f in inventory],
        "cases": [], "notes": [
            "File sizes are disk bytes, not resident memory.",
            "RSS, physical footprint, MLX active memory and VRAM overlap; do not sum them.",
            "Capacity-fill uses teacher-forced prefill; maximum autoregressive output peak is not measured.",
            "No workload totals or automatic reservation changes are produced."]}
    directory = Path(args.output_dir) / model["id"] / engine / artifact["id"]
    target = directory / "memory-profile.json"
    for kv_type in args.kv_types:
        row = {"kv_type_requested": kv_type, "status": "planned"}
        report["cases"].append(row)
        if not args.execute:
            continue
        case_dir = directory / kv_type
        case_dir.mkdir(parents=True, exist_ok=True)
        row["status"] = "running"
        save(target, with_gib(report))
        print(f"START {model['id']} {engine} {kv_type} context={budget['total_tokens']}", flush=True)
        try:
            if pressure() in (2, 4) or (available_bytes() is not None and available_bytes() < GIB):
                raise MemoryError("system memory is already under pressure")
            main = next(f for f in inventory if f["role"] == "model")
            if any(not Path(f["path"]).exists() for f in inventory):
                raise FileNotFoundError("artifact component not cached; run Mica setup first")
            row["checksum_verified_roles"] = []
            for entry in inventory:
                if entry.get("sha256") and Path(entry["path"]).is_file():
                    with Path(entry["path"]).open("rb") as stream:
                        digest = hashlib.file_digest(stream, "sha256").hexdigest()
                    if digest != entry["sha256"]:
                        raise ValueError("cached component checksum mismatch")
                    row["checksum_verified_roles"].append(entry["role"])
            if engine not in ("mlx-lm", "mlx-vlm", "llama-cpp"):
                raise ValueError("no validated profiling adapter for this engine; do not substitute stock llama.cpp")
            if artifact["format"] == "mlx":
                row["main"] = run_mlx(Path(main["path"]), kv_type, budget, args, case_dir)
            elif artifact["format"] == "gguf":
                row["main"] = run_llama(args.llama_binary, Path(main["path"]), kv_type, budget, args, case_dir)
                projector = next((f for f in inventory if f["role"] == "vision-projector"), None)
                if projector and row["main"]["status"] == "measured":
                    row["projector"] = run_llama(args.llama_binary, Path(main["path"]), kv_type, budget,
                        args, case_dir, Path(projector["path"]))
                    if row["projector"]["status"] == "measured":
                        before, after = row["main"]["snapshots"][0], row["projector"]["snapshots"][0]
                        row["projector"]["incremental_loaded_bytes"] = {key: after[key] - before[key]
                            for key in ("rss_bytes", "physical_footprint_bytes", "nvidia_vram_bytes")
                            if after.get(key) is not None and before.get(key) is not None}
                        row["projector"]["component_measurement"] = "incremental paired worker; not standalone total"
                row["drafters"] = []
                for draft in [f for f in inventory if f["role"] in ("mtp-drafter", "dflash-drafter")]:
                    draft_dir = case_dir / draft["role"]
                    draft_dir.mkdir(exist_ok=True)
                    # Never attach to the target here: no speculative-mode totals.
                    measured = run_llama(args.llama_binary, Path(draft["path"]), kv_type, budget, args, draft_dir)
                    measured["role"] = draft["role"]
                    measured["compatibility_with_target_measured"] = False
                    row["drafters"].append(measured)
            else:
                row["main"] = {"status": "unsupported", "error": "no adapter for this artifact format"}
            components = [row["main"], *row.get("drafters", [])]
            if "projector" in row:
                components.append(row["projector"])
            row["status"] = ("measured" if all(c["status"] == "measured" for c in components)
                else row["main"]["status"] if row["main"]["status"] != "measured" else "partial")
        except MemoryError as error:
            row.update(status="guard_stopped", error=str(error))
        except KeyboardInterrupt:
            row.update(status="interrupted", error="user interruption; worker terminated")
            save(target, with_gib(report))
            raise
        except Exception as error:
            row.update(status="failed", error=type(error).__name__ + ": " + str(error))
        save(target, with_gib(report))
        print(f"FINISH {model['id']} {kv_type}: {row['status']}", flush=True)
    save(target, with_gib(report))
    return report


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--mlx-worker":
        return mlx_worker(sys.argv[2], sys.argv[3])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, action="append", required=True)
    parser.add_argument("--artifact", default="q4")
    parser.add_argument("--engine", help="otherwise first compatible engine for first matching artifact")
    parser.add_argument("--kv-types", nargs="+", choices=list(KV_TYPES), default=["q4", "q8"])
    parser.add_argument("--input-tokens", type=int, help="otherwise declared total minus output reserve")
    parser.add_argument("--output-tokens", type=int, help="explicit measurement reserve; not a training claim")
    parser.add_argument("--cache-root", type=Path, default=Path.home() / ".mica")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--llama-binary", type=Path)
    parser.add_argument("--mlx-python", type=Path)
    parser.add_argument("--mlx-cache-path", choices=["batch", "stream"], default="batch",
                        help="match mlx-vlm server batch cache (one row), or its single-stream cache")
    parser.add_argument("--limit-gib", type=float, default=12)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--gpu-layers", type=int, default=99)
    parser.add_argument("--threads", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--batch-size", type=int, default=512)
    parser.add_argument("--prefill-chunk", type=int, default=128)
    parser.add_argument("--image-tokens", type=int, default=1024)
    parser.add_argument("--decode-tokens", type=int, default=32)
    parser.add_argument("--execute", action="store_true", help="otherwise only write a plan; never download/install")
    args = parser.parse_args()
    if (not 0 < args.limit_gib <= 16 or args.timeout <= 0 or args.prefill_chunk <= 0
            or args.batch_size < args.prefill_chunk or args.threads < 1 or args.decode_tokens < 0):
        parser.error("positive limits required; ceiling <=16 GiB; batch >= prefill chunk; decode >=0")
    args.cache_root = args.cache_root.expanduser().resolve()
    args.output_dir = args.output_dir or args.cache_root / "measurements/model-memory"
    args.llama_binary = args.llama_binary or args.cache_root / "runtimes/llama.cpp/build-mica/bin/llama-server"
    args.mlx_python = args.mlx_python or args.cache_root / "environments/mlx/bin/python"
    failed = False
    for manifest in args.manifest:
        report = profile_one(manifest, args)
        failed |= args.execute and any(c["status"] != "measured" for c in report["cases"])
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

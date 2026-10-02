#!/usr/bin/env python3
"""Emit mica-server's versioned, dependency-free hardware profile."""

from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any

GIB = 1024**3


def number(value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def run(*args: str) -> str:
    try:
        return subprocess.run(
            args, check=False, capture_output=True, text=True, timeout=10
        ).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def sysctl(name: str) -> str:
    return run("sysctl", "-n", name)


def mac_hardware() -> dict[str, Any]:
    if platform.system() != "Darwin":
        return {}
    try:
        return json.loads(
            run("system_profiler", "SPHardwareDataType", "-json")
        ).get("SPHardwareDataType", [{}])[0]
    except (json.JSONDecodeError, IndexError, TypeError):
        return {}


def system_ram_gib() -> float:
    if platform.system() == "Darwin":
        try:
            return round(int(sysctl("hw.memsize")) / GIB, 3)
        except ValueError:
            match = re.search(r"([0-9.]+)\s*(GB|TB)", str(mac_hardware().get("physical_memory", "")))
            if match:
                scale = 1024 if match.group(2) == "TB" else 1
                return round(float(match.group(1)) * scale, 3)
    try:
        pages = os.sysconf("SC_PHYS_PAGES")
        page_size = os.sysconf("SC_PAGE_SIZE")
        return round(pages * page_size / GIB, 3)
    except (ValueError, OSError):
        return 0.0


def cpu_profile() -> dict[str, Any]:
    logical = os.cpu_count() or 0
    physical = 0
    vendor = ""
    model = platform.processor()
    if platform.system() == "Darwin":
        vendor = "apple" if platform.machine() in {"arm64", "aarch64"} else "intel"
        overview = mac_hardware()
        model = (
            sysctl("machdep.cpu.brand_string")
            or str(overview.get("chip_type", ""))
            or sysctl("hw.model")
            or model
        )
        try:
            physical = int(sysctl("hw.physicalcpu"))
            logical = int(sysctl("hw.logicalcpu"))
        except ValueError:
            counts = re.findall(r"\d+", str(overview.get("number_processors", "")))
            if counts:
                physical = logical = int(counts[0])
    elif Path("/proc/cpuinfo").exists():
        values: dict[str, str] = {}
        for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                values.setdefault(key.strip(), value.strip())
        vendor = values.get("vendor_id", values.get("CPU implementer", ""))
        model = values.get("model name", values.get("Hardware", model))
        topology = run("lscpu", "-p=SOCKET,CORE")
        cores = {line for line in topology.splitlines() if line and not line.startswith("#")}
        physical = len(cores)
    return {
        "vendor": vendor.lower(),
        "model": model,
        "physical_cores": physical,
        "logical_cores": logical,
    }


def add_device(devices: list[dict[str, Any]], candidate: dict[str, Any]) -> None:
    key = (candidate.get("runtime"), candidate.get("id"), candidate.get("name"))
    if any((d.get("runtime"), d.get("id"), d.get("name")) == key for d in devices):
        return
    devices.append(candidate)


def detect_apple(devices: list[dict[str, Any]], ram_gib: float) -> None:
    if platform.system() != "Darwin" or platform.machine() not in {"arm64", "aarch64"}:
        return
    name = sysctl("machdep.cpu.brand_string") or sysctl("hw.model") or "Apple Silicon"
    cores = ""
    raw = run("system_profiler", "SPDisplaysDataType", "-json")
    try:
        display = json.loads(raw).get("SPDisplaysDataType", [{}])[0]
        name = display.get("sppci_model", display.get("_name", name))
        cores = str(display.get("sppci_cores", ""))
    except (json.JSONDecodeError, IndexError, TypeError):
        pass
    add_device(
        devices,
        {
            "id": "0",
            "type": "gpu",
            "vendor": "apple",
            "name": name,
            "runtime": "metal",
            "architecture": f"{cores}-core" if cores else platform.machine(),
            "driver": platform.mac_ver()[0],
            "memory_gib": ram_gib,
            "unified_memory": True,
            "apis": ["metal"],
        },
    )


def detect_nvidia(devices: list[dict[str, Any]]) -> None:
    output = run(
        "nvidia-smi",
        "--query-gpu=index,name,memory.total,driver_version,compute_cap",
        "--format=csv,noheader,nounits",
    )
    if not output:
        output = run(
            "nvidia-smi",
            "--query-gpu=index,name,memory.total,driver_version",
            "--format=csv,noheader,nounits",
        )
    for line in output.splitlines():
        fields = [part.strip() for part in line.split(",")]
        if len(fields) < 4:
            continue
        try:
            memory = round(float(fields[2]) / 1024, 3)
        except ValueError:
            memory = 0.0
        add_device(
            devices,
            {
                "id": fields[0],
                "type": "gpu",
                "vendor": "nvidia",
                "name": fields[1],
                "runtime": "cuda",
                "architecture": f"compute_{fields[4]}" if len(fields) > 4 else "",
                "driver": fields[3],
                "memory_gib": memory,
                "unified_memory": False,
                "apis": ["cuda", "vulkan"],
            },
        )


def detect_amd(devices: list[dict[str, Any]]) -> None:
    raw = run("rocm-smi", "--showproductname", "--showmeminfo", "vram", "--json")
    try:
        cards = json.loads(raw)
    except json.JSONDecodeError:
        cards = {}
    architectures = re.findall(r"\bgfx[0-9a-f]+\b", run("rocminfo"), re.IGNORECASE)
    for index, (card, values) in enumerate(cards.items()):
        if not isinstance(values, dict):
            continue
        name = next(
            (str(v) for k, v in values.items() if "series" in k.lower() or "model" in k.lower()),
            "AMD GPU",
        )
        total_bytes = next(
            (
                float(v)
                for k, v in values.items()
                if "vram" in k.lower() and "total" in k.lower() and str(v).replace(".", "", 1).isdigit()
            ),
            0.0,
        )
        add_device(
            devices,
            {
                "id": card,
                "type": "gpu",
                "vendor": "amd",
                "name": name,
                "runtime": "rocm",
                "architecture": architectures[index] if index < len(architectures) else "",
                "driver": run("hipconfig", "--version"),
                "memory_gib": round(total_bytes / GIB, 3),
                "unified_memory": False,
                "apis": ["rocm", "hip", "vulkan"],
            },
        )


def detect_intel_xpu(devices: list[dict[str, Any]]) -> None:
    raw = run("xpu-smi", "discovery", "-j")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        payload = {}
    rows = payload.get("device_list", payload.get("devices", []))
    for index, row in enumerate(rows if isinstance(rows, list) else []):
        if not isinstance(row, dict):
            continue
        add_device(
            devices,
            {
                "id": str(row.get("device_id", index)),
                "type": "gpu",
                "vendor": "intel",
                "name": str(row.get("device_name", row.get("name", "Intel GPU"))),
                "runtime": "xpu",
                "architecture": str(row.get("pci_bdf_address", "")),
                "driver": str(row.get("driver_version", "")),
                "memory_gib": round(
                    number(row.get("memory_physical_size_byte", 0)) / GIB, 3
                ),
                "unified_memory": False,
                "apis": ["xpu", "sycl", "level-zero", "vulkan"],
            },
        )


def detect_tpu(devices: list[dict[str, Any]]) -> None:
    nodes = sorted(glob.glob("/dev/accel*"))
    tpu_name = os.environ.get("TPU_NAME", "")
    if not nodes and not tpu_name:
        return
    for index, node in enumerate(nodes or [tpu_name]):
        add_device(
            devices,
            {
                "id": str(index),
                "type": "tpu",
                "vendor": "google",
                "name": tpu_name or f"Google TPU ({Path(node).name})",
                "runtime": "tpu",
                "architecture": os.environ.get("TPU_ACCELERATOR_TYPE", ""),
                "driver": "",
                "memory_gib": 0.0,
                "unified_memory": False,
                "apis": ["jax", "xla"],
            },
        )


def detect_pci_fallbacks(devices: list[dict[str, Any]]) -> None:
    if platform.system() != "Linux":
        return
    output = run("lspci", "-D")
    existing_vendors = {device["vendor"] for device in devices}
    for line in output.splitlines():
        lowered = line.lower()
        if not any(word in lowered for word in ("vga", "3d controller", "display controller")):
            continue
        slot = line.split()[0] if line.split() else "unknown"
        if "amd" in lowered and "amd" not in existing_vendors:
            add_device(devices, {"id": slot, "type": "gpu", "vendor": "amd", "name": line,
                        "runtime": "unavailable", "architecture": "", "driver": "",
                        "memory_gib": 0.0, "unified_memory": False, "apis": []})
        if "intel" in lowered and "intel" not in existing_vendors:
            add_device(devices, {"id": slot, "type": "gpu", "vendor": "intel", "name": line,
                        "runtime": "unavailable", "architecture": "", "driver": "",
                        "memory_gib": 0.0, "unified_memory": False, "apis": []})


def backend_targets(
    devices: list[dict[str, Any]], available: dict[str, bool]
) -> dict[str, dict[str, Any]]:
    runtimes = {device["runtime"] for device in devices}
    gguf = "cpu"
    audio = "cpu"
    vllm = "cpu"
    if "metal" in runtimes:
        vllm = "metal"
        if available.get("metal"):
            gguf = audio = "metal"
    elif "cuda" in runtimes:
        vllm = "cuda"
        if available.get("cuda"):
            gguf = audio = "cuda"
        elif available.get("vulkan"):
            gguf = audio = "vulkan"
    elif "rocm" in runtimes:
        vllm = "rocm"
        if available.get("rocm"):
            gguf = audio = "hip"
        elif available.get("vulkan"):
            gguf = audio = "vulkan"
    elif "xpu" in runtimes:
        vllm = "xpu"
        if available.get("xpu"):
            gguf = "sycl"
        elif available.get("vulkan"):
            gguf = "vulkan"
        if available.get("vulkan"):
            audio = "vulkan"
    elif "tpu" in runtimes:
        vllm = "tpu"
    return {
        "mlx": {"device": "metal" if "metal" in runtimes else "unsupported"},
        "gguf": {"device": gguf},
        "audio": {"device": audio},
        "vllm": {"device": vllm},
    }


def toolchains() -> dict[str, bool]:
    return {
        name: shutil.which(executable) is not None
        for name, executable in {
            "cmake": "cmake",
            "cxx": os.environ.get("CXX", "c++"),
            "python": "python3",
            "uv": "uv",
            "cuda": "nvcc",
            "rocm": "hipcc",
            "xpu": "icpx",
            "vulkan": "vulkaninfo",
            "metal": "xcrun",
        }.items()
    }


def build_profile() -> dict[str, Any]:
    ram_gib = system_ram_gib()
    system_name = platform.system()
    release = platform.release()
    version = platform.mac_ver()[0] if system_name == "Darwin" else release
    is_wsl = system_name == "Linux" and "microsoft" in platform.uname().release.lower()
    os_name = "macos" if system_name == "Darwin" else ("wsl" if is_wsl else system_name.lower())
    apple_silicon = os_name == "macos" and platform.machine() in {"arm64", "aarch64"}
    devices: list[dict[str, Any]] = []
    detect_apple(devices, ram_gib)
    detect_nvidia(devices)
    detect_amd(devices)
    detect_intel_xpu(devices)
    detect_tpu(devices)
    detect_pci_fallbacks(devices)
    available_toolchains = toolchains()
    return {
        "schema": 1,
        "system": {
            "os": os_name,
            "version": version,
            "arch": platform.machine(),
            "wsl": is_wsl,
            "apple_silicon": apple_silicon,
        },
        "cpu": cpu_profile(),
        "memory": {
            "system_ram_gib": ram_gib,
            "unified_memory_gib": ram_gib if apple_silicon else 0.0,
        },
        "accelerators": devices,
        "toolchains": available_toolchains,
        "build_policy": {"max_memory_gib": 16, "max_parallel_jobs": 2},
        "backend_targets": backend_targets(devices, available_toolchains),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(build_profile(), indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

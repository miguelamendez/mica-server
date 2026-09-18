#!/usr/bin/env python3
"""Create portable compressed-tensors candidates for later vLLM validation.

This script deliberately does not mark an artifact deployable. The C++ control
plane stores outputs below checkpoints/vllm/<model>/candidates; promotion only
happens after real inference and quality comparison on target hardware.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def load_profile(path: Path, model_id: str, quant: str,
                 capability: str) -> tuple[dict, dict, dict]:
    config = json.loads(path.read_text())
    if config.get("schema") != 1:
        raise ValueError("unsupported vLLM quantization profile schema")
    try:
        defaults = config["defaults"][quant]
    except KeyError as error:
        raise ValueError(f"missing vLLM quantization profile field: {error}") from error
    model = config["models"].get(model_id, {
        "capability": capability,
        "ignore_layers": ["lm_head"],
        "calibration": "chat-text" if capability == "text" else "custom-required",
        "runtime_status": "candidate-only",
        "runtime_note": "A custom model requires architecture-specific vLLM validation.",
    })
    return config, defaults, model


def dataset_spec(args: argparse.Namespace, config: dict) -> tuple[str, str]:
    dataset = args.dataset or config["defaults"]["dataset"]
    split = args.dataset_split or config["defaults"]["dataset_split"]
    return dataset, split


def auto_round_command(args: argparse.Namespace, config: dict, defaults: dict,
                       model: dict) -> list[str]:
    executable = shutil.which("auto-round") or ("auto-round" if args.plan else None)
    if executable is None:
        raise RuntimeError("auto-round executable is unavailable")
    dataset, _ = dataset_spec(args, config)
    command = [
        executable,
        "--model_name", str(args.source),
        "--output_dir", str(args.output),
        "--format", config["format"],
        "--algorithm", "auto_round",
        "--scheme", args.scheme,
        "--group_size", str(config["group_size"]),
        "--dataset", dataset,
        "--nsamples", str(args.calibration_samples),
        "--seqlen", str(args.calibration_sequence_length),
        "--batch_size", str(args.batch_size),
        "--iters", str(args.iterations),
        "--device_map", args.device,
        "--ignore_layers", ",".join(model["ignore_layers"]),
        "--enable_deterministic_algorithms",
        "--low_gpu_mem_usage",
    ]
    return command


def prepare_text_dataset(tokenizer, args: argparse.Namespace, config: dict):
    from datasets import load_dataset

    dataset_name, split_name = dataset_spec(args, config)
    dataset_path = Path(dataset_name)
    if dataset_path.is_file() and dataset_path.suffix in {".json", ".jsonl"}:
        split = f"train[:{args.calibration_samples}]"
        dataset = load_dataset(
            "json", data_files=str(dataset_path.resolve()), split=split
        )
    else:
        split = f"{split_name}[:{args.calibration_samples}]"
        dataset = load_dataset(dataset_name, split=split)
    dataset = dataset.shuffle(seed=config["defaults"]["seed"])

    def preprocess(sample):
        if "messages" in sample:
            text = tokenizer.apply_chat_template(
                sample["messages"], tokenize=False, add_generation_prompt=False
            )
        elif "text" in sample:
            text = sample["text"]
        else:
            raise ValueError("calibration dataset needs a messages or text column")
        return tokenizer(
            text,
            padding=False,
            max_length=args.calibration_sequence_length,
            truncation=True,
            add_special_tokens=False,
        )

    return dataset.map(preprocess, remove_columns=dataset.column_names)


def run_gptq(args: argparse.Namespace, config: dict, model_profile: dict) -> dict:
    import torch

    # LLM Compressor 0.11 selects MPS whenever it is visible, even when the
    # caller explicitly requested CPU. Its sequential cache then tries to
    # replace CPU tensor storage with MPS storage. Keep the requested device
    # authoritative; CUDA/XPU paths are unaffected.
    if args.device == "cpu":
        if hasattr(torch, "mps"):
            torch.mps.is_available = lambda: False
        if hasattr(torch, "accelerator"):
            torch.accelerator.is_available = lambda: False

    from llmcompressor import oneshot
    from llmcompressor.modifiers.gptq import GPTQModifier
    from transformers import (
        AutoModel,
        AutoModelForCausalLM,
        AutoModelForCTC,
        AutoModelForImageTextToText,
        AutoProcessor,
        AutoTokenizer,
    )

    capability = model_profile["capability"]
    loaders = {
        "text": AutoModelForCausalLM,
        "vision": AutoModelForImageTextToText,
        "asr": AutoModelForCTC,
        "tts": AutoModel,
    }
    device_map = "auto" if args.device in {"cuda", "xpu", "auto"} else args.device
    model_kwargs = {
        "torch_dtype": "auto",
        "device_map": device_map,
        "low_cpu_mem_usage": True,
    }
    processor = None
    tokenizer = None
    loader = loaders[capability]
    quantized_model = loader.from_pretrained(
        args.source, trust_remote_code=True, **model_kwargs
    )
    if args.model_id != "granite-speech-5":
        tokenizer = AutoTokenizer.from_pretrained(args.source, trust_remote_code=True)
    calibration_metadata = {
        "dataset": dataset_spec(args, config)[0],
        "dataset_split": dataset_spec(args, config)[1],
    }
    if capability == "text":
        processor = tokenizer
        dataset = prepare_text_dataset(tokenizer, args, config)
        local_dataset = Path(dataset_spec(args, config)[0])
        if local_dataset.is_file():
            calibration_metadata = {
                "manifest": str(local_dataset.resolve()),
                "manifest_sha256": hashlib.sha256(
                    local_dataset.read_bytes()
                ).hexdigest(),
                "records_used": len(dataset),
                "sample_mix": {"chat": len(dataset)},
            }
    else:
        if not args.dataset:
            raise ValueError(
                f"{args.model_id} requires --dataset pointing to an audited JSONL "
                f"{capability} calibration manifest"
            )
        from vllm_calibration import build_manifest_dataloader

        if processor is None:
            processor = AutoProcessor.from_pretrained(args.source, trust_remote_code=True)
        dataset, calibration_metadata = build_manifest_dataloader(
            Path(args.dataset), capability, processor, quantized_model,
            args.calibration_samples, args.calibration_sequence_length, args.batch_size,
        )
    def gptq_ignore(pattern: str) -> str:
        if "*" not in pattern:
            return pattern
        return "re:" + re.escape(pattern).replace(r"\*", ".*")

    weight_bits = 4 if args.scheme == "W4A16" else 8
    explicit_scheme = {
        "targets": ["Linear"],
        "weights": {
            "num_bits": weight_bits,
            "type": "int",
            "symmetric": True,
            "strategy": "group",
            "group_size": int(config["group_size"]),
            # The current compressed-tensors alpha presets leave this field
            # unset. Pin it explicitly so Granite's Transformers-5.17
            # toolchain and the stable toolchains produce the same recipe.
            "observer": "memoryless_minmax",
        }
    }
    recipe = GPTQModifier(
        config_groups={"group_0": explicit_scheme},
        ignore=[gptq_ignore(pattern) for pattern in model_profile["ignore_layers"]],
    )
    processor_arguments = ({"tokenizer": tokenizer} if capability == "text"
                           else {"processor": processor})
    oneshot(
        model=quantized_model,
        **processor_arguments,
        dataset=dataset,
        recipe=recipe,
        max_seq_length=args.calibration_sequence_length,
        num_calibration_samples=args.calibration_samples,
        batch_size=args.batch_size,
        sequential_offload_device="cpu",
    )
    args.output.mkdir(parents=True, exist_ok=True)
    quantized_model.save_pretrained(args.output, save_compressed=True)
    if tokenizer is not None:
        tokenizer.save_pretrained(args.output)
    if capability != "text":
        processor.save_pretrained(args.output)
    del quantized_model
    if torch.cuda.is_available():
        torch.cuda.empty_cache()
    return calibration_metadata


def write_manifest(args: argparse.Namespace, config: dict, defaults: dict,
                   model: dict, calibration_metadata: dict | None = None) -> None:
    tool_versions = {}
    for package in (
        "auto-round",
        "compressed-tensors",
        "llmcompressor",
        "torch",
        "transformers",
    ):
        try:
            tool_versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            tool_versions[package] = None
    manifest = {
        "schema": 1,
        "status": "candidate-unvalidated",
        "model": args.model_id,
        "source": str(args.source),
        "source_repo": args.source_repo,
        "source_revision": args.source_revision,
        "source_license": args.source_license,
        "algorithm": args.algorithm,
        "toolchain": model.get("toolchain", "current"),
        "scheme": args.scheme,
        "format": config["format"],
        "group_size": config["group_size"],
        "calibration_samples": args.calibration_samples,
        "calibration_sequence_length": args.calibration_sequence_length,
        "calibration_dataset": dataset_spec(args, config)[0],
        "calibration_dataset_split": dataset_spec(args, config)[1],
        "calibration_mode": model["calibration"],
        "calibration_evidence": calibration_metadata or {},
        "protected_layers": model["ignore_layers"],
        "target_device": args.device,
        "runtime_status": model["runtime_status"],
        "runtime_note": model["runtime_note"],
        "tool_versions": tool_versions,
        "acceptance": {
            "reference_comparison": False,
            "vllm_load": False,
            "real_inference": False,
            "quality": False,
            "performance": False,
        },
    }
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "mica-vllm-candidate.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--capability", choices=("text", "vision", "asr", "tts"), required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-repo", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--source-license", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quant", choices=("q4", "q8"), required=True)
    parser.add_argument("--algorithm", choices=("auto_round", "gptq"), required=True)
    parser.add_argument("--scheme", choices=("W4A16", "W8A16"), required=True)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--dataset")
    parser.add_argument("--dataset-split")
    parser.add_argument("--calibration-samples", type=int, required=True)
    parser.add_argument("--calibration-sequence-length", type=int, required=True)
    parser.add_argument("--batch-size", type=int, required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--plan", action="store_true")
    args = parser.parse_args()
    if args.quant == "q4" and args.scheme != "W4A16":
        parser.error("q4 requires W4A16")
    if args.quant == "q8" and args.scheme != "W8A16":
        parser.error("q8 requires W8A16")
    for name in ("calibration_samples", "calibration_sequence_length", "batch_size", "iterations"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    return args


def main() -> int:
    args = parse_args()
    config, defaults, model = load_profile(
        args.profiles, args.model_id, args.quant, args.capability
    )
    if args.plan:
        payload = {
            "model": args.model_id,
            "algorithm": args.algorithm,
            "scheme": args.scheme,
            "format": config["format"],
            "protected_layers": model["ignore_layers"],
            "runtime_status": model["runtime_status"],
        }
        if args.algorithm == "auto_round":
            payload["command"] = auto_round_command(args, config, defaults, model)
        print(json.dumps(payload, indent=2))
        return 0

    if model["calibration"] == "custom-required":
        raise RuntimeError(
            f"{args.model_id} requires a modality-specific calibration adapter; "
            "text-only calibration is intentionally refused"
        )
    if model["calibration"] != "chat-text" and not args.dataset:
        raise RuntimeError(
            f"{args.model_id} requires --dataset pointing to an audited JSONL "
            "modality calibration manifest"
        )

    if args.output.exists() and any(args.output.iterdir()):
        raise RuntimeError(f"candidate output is not empty: {args.output}")
    calibration_metadata = None
    if args.algorithm == "auto_round":
        if model["calibration"] != "chat-text":
            raise RuntimeError(
                "AutoRound CLI does not preserve Mica's modality manifest inputs; "
                "use GPTQ for multimodal/audio candidates"
            )
        subprocess.run(auto_round_command(args, config, defaults, model), check=True)
    else:
        calibration_metadata = run_gptq(args, config, model)
    write_manifest(args, config, defaults, model, calibration_metadata)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"vllm_quantize.py: {error}", file=sys.stderr)
        if os.environ.get("MICA_DEBUG") == "1":
            raise
        raise SystemExit(1)

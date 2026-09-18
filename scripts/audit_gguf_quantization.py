#!/usr/bin/env python3
"""Audit and compare GGUF tensor precision policies without loading model weights.

The script depends only on llama.cpp's ``gguf-py`` package. Point PYTHONPATH at
``<llama.cpp>/gguf-py`` when it is not installed in the active environment.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from gguf import GGMLQuantizationType, GGUFReader


def tensor_family(name: str) -> str:
    """Return a stable, architecture-neutral tensor family."""
    lowered = name.lower()
    if "token_embd" in lowered or "embed_tokens" in lowered:
        return "token_embedding"
    if lowered.startswith("output.") or "lm_head" in lowered:
        return "output_head"
    if "norm" in lowered:
        return "normalization"
    if "mmproj" in lowered or "vision" in lowered or "v.patch" in lowered:
        return "vision_or_projector"
    if "attn_qkv" in lowered:
        return "attention_qkv"
    if ".attn_q." in lowered or ".attn_k." in lowered or ".attn_v." in lowered:
        return "attention_qkv"
    if "attn_output" in lowered or ".attn_o." in lowered:
        return "attention_output"
    if "ffn_down" in lowered:
        return "ffn_down"
    if "ffn_gate" in lowered or "ffn_up" in lowered:
        return "ffn_gate_up"
    if "conv" in lowered or "codec" in lowered or "audio" in lowered:
        return "audio_or_codec"
    return "other"


def audit(path: Path) -> dict[str, Any]:
    reader = GGUFReader(path, mode="r")
    type_counts: Counter[str] = Counter()
    type_elements: Counter[str] = Counter()
    family_types: dict[str, Counter[str]] = defaultdict(Counter)
    tensors: dict[str, dict[str, Any]] = {}

    for tensor in reader.tensors:
        quant_type = GGMLQuantizationType(tensor.tensor_type).name
        elements = int(tensor.n_elements)
        family = tensor_family(tensor.name)
        type_counts[quant_type] += 1
        type_elements[quant_type] += elements
        family_types[family][quant_type] += 1
        tensors[tensor.name] = {
            "type": quant_type,
            "shape": [int(part) for part in tensor.shape],
            "elements": elements,
            "family": family,
        }

    return {
        "path": str(path.resolve()),
        "size_bytes": path.stat().st_size,
        "tensor_count": len(tensors),
        "type_counts": dict(sorted(type_counts.items())),
        "type_element_percent": {
            key: round(value * 100 / sum(type_elements.values()), 4)
            for key, value in sorted(type_elements.items())
        },
        "family_types": {
            family: dict(sorted(counts.items()))
            for family, counts in sorted(family_types.items())
        },
        "tensors": tensors,
    }


def compare(left: dict[str, Any], right: dict[str, Any]) -> dict[str, Any]:
    left_tensors = left["tensors"]
    right_tensors = right["tensors"]
    shared = sorted(set(left_tensors) & set(right_tensors))
    type_differences = [
        {
            "tensor": name,
            "left": left_tensors[name]["type"],
            "right": right_tensors[name]["type"],
        }
        for name in shared
        if left_tensors[name]["type"] != right_tensors[name]["type"]
    ]
    shape_differences = [
        name
        for name in shared
        if left_tensors[name]["shape"] != right_tensors[name]["shape"]
    ]
    return {
        "left": left["path"],
        "right": right["path"],
        "same_tensor_names": set(left_tensors) == set(right_tensors),
        "same_shapes": not shape_differences,
        "same_precision_map": not type_differences
        and set(left_tensors) == set(right_tensors),
        "left_only": sorted(set(left_tensors) - set(right_tensors)),
        "right_only": sorted(set(right_tensors) - set(left_tensors)),
        "shape_differences": shape_differences,
        "type_differences": type_differences,
    }


def gib(value: int) -> str:
    return f"{value / math.pow(1024, 3):.3f}"


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# GGUF quantization audit",
        "",
        "| File | GiB | Tensors | Tensor types |",
        "| --- | ---: | ---: | --- |",
    ]
    for item in report["files"]:
        types = ", ".join(f"{key}: {value}" for key, value in item["type_counts"].items())
        lines.append(
            f"| `{Path(item['path']).name}` | {gib(item['size_bytes'])} | "
            f"{item['tensor_count']} | {types} |"
        )

    lines += ["", "## Precision policy by tensor family", ""]
    for item in report["files"]:
        lines += [f"### `{Path(item['path']).name}`", "", "| Family | Tensor types |", "| --- | --- |"]
        for family, types in item["family_types"].items():
            rendered = ", ".join(f"{key}: {value}" for key, value in types.items())
            lines.append(f"| {family} | {rendered} |")
        lines.append("")

    if report["comparisons"]:
        lines += ["## Pairwise comparison", "", "| Left | Right | Names | Shapes | Precision map |", "| --- | --- | --- | --- | --- |"]
        for item in report["comparisons"]:
            yes_no = lambda value: "same" if value else "different"
            lines.append(
                f"| `{Path(item['left']).name}` | `{Path(item['right']).name}` | "
                f"{yes_no(item['same_tensor_names'])} | {yes_no(item['same_shapes'])} | "
                f"{yes_no(item['same_precision_map'])} |"
            )
        lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gguf", type=Path, nargs="+", help="GGUF files to audit")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    files = [audit(path) for path in args.gguf]
    report = {
        "schema_version": 1,
        "files": files,
        "comparisons": [
            compare(files[index], files[index + 1])
            for index in range(len(files) - 1)
        ],
    }
    rendered_json = json.dumps(report, indent=2) + "\n"
    rendered_markdown = markdown(report) + "\n"
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered_json)
    if args.markdown_out:
        args.markdown_out.parent.mkdir(parents=True, exist_ok=True)
        args.markdown_out.write_text(rendered_markdown)
    if not args.json_out and not args.markdown_out:
        print(rendered_json, end="")
    else:
        print(rendered_markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

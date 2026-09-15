#!/usr/bin/env python3
"""Mica MLX-audio conversion profiles and artifact model cards."""

import argparse
import json
from pathlib import Path


def load_profile(path: str, profile: str) -> dict:
    document = json.loads(Path(path).read_text())
    try:
        return document["profiles"][profile]
    except KeyError as error:
        raise ValueError(f"unknown MLX-audio quantization profile: {profile}") from error


def apply_profile(profile: str, specification: dict) -> None:
    preserved = tuple(specification["preserve_prefixes"])

    if profile == "granite-speech5-quality":
        from mlx_audio.stt.models.granite_speech5_ctc.granite_speech5 import Model
    elif profile == "audio8-quality":
        from mlx_audio.tts.models.arktts.arktts import Model
    else:
        raise ValueError(f"profile has no converter implementation: {profile}")

    original = getattr(Model, "model_quant_predicate", lambda self, path, module: True)

    def quality_predicate(self, path, module):
        return original(self, path, module) and not path.startswith(preserved)

    Model.model_quant_predicate = quality_predicate


def write_model_card(args, specification: dict, config: dict) -> None:
    quantization = config.get("quantization", {})
    bits = quantization.get("bits", args.q_bits)
    group_size = quantization.get("group_size", args.q_group_size)
    preserved = specification["preserve_prefixes"]
    reasons = specification["reasons"]

    metadata = {
        "profile": args.profile,
        "status": specification["status"],
        "preserved_modules": preserved,
        "preservation_reasons": reasons,
        "validation": specification["validation"],
    }
    config["mica_quantization"] = metadata

    card = [
        "---",
        f"license: {args.source_license}",
        f"base_model: {args.source_repo}",
        "tags:",
        "- mlx",
        "- quantized",
        "- audio",
        "---",
        "",
        f"# {args.model_id} MLX {bits}-bit",
        "",
        f"This artifact was converted from [{args.source_repo}](https://huggingface.co/{args.source_repo})",
        f"at immutable revision `{args.source_revision}`.",
        "",
        "## Quantization",
        "",
        f"- Format: MLX affine {bits}-bit",
        f"- Group size: {group_size}",
        f"- Mica profile: `{args.profile}`",
        f"- Policy status: {specification['status']}",
        "",
        "The following module prefixes remain at source precision:",
        "",
    ]
    card.extend(f"- `{module}`" for module in preserved)
    card.extend(["", "Reasons:", ""])
    card.extend(f"- {reason}" for reason in reasons)
    card.extend(
        [
            "",
            "## Validation",
            "",
            specification["validation"],
            "",
            "Preservation is reported separately from validation: a protected module may be",
            "architecture-guided without yet being proven necessary by an individual ablation.",
            "",
        ]
    )
    Path(args.mlx_path, "README.md").write_text("\n".join(card))

    repository_root = Path(args.mlx_path).parent
    variants = []
    for name in ("q4", "q8"):
        candidate = repository_root / name / "config.json"
        if not candidate.exists():
            continue
        candidate_config = json.loads(candidate.read_text())
        candidate_quant = candidate_config.get("quantization", {})
        variants.append(
            (
                name.upper(),
                candidate_quant.get("bits", "unknown"),
                candidate_quant.get("group_size", "unknown"),
            )
        )

    repository_card = [
        "---",
        f"license: {args.source_license}",
        f"base_model: {args.source_repo}",
        "tags:",
        "- mlx",
        "- quantized",
        "- audio",
        "---",
        "",
        f"# {args.model_id} MLX",
        "",
        f"MLX conversions of [{args.source_repo}](https://huggingface.co/{args.source_repo})",
        f"from immutable revision `{args.source_revision}`.",
        "",
        "## Variants",
        "",
        "| Path | Bits | Group size |",
        "|---|---:|---:|",
    ]
    repository_card.extend(
        f"| [`{name.lower()}`](./{name.lower()}) | {variant_bits} | {variant_group} |"
        for name, variant_bits, variant_group in variants
    )
    repository_card.extend(
        [
            "",
            "## Selective quantization policy",
            "",
            f"Profile: `{args.profile}`",
            "",
            f"Status: {specification['status']}",
            "",
            "The following module prefixes remain at source precision:",
            "",
        ]
    )
    repository_card.extend(f"- `{module}`" for module in preserved)
    repository_card.extend(["", "Reasons:", ""])
    repository_card.extend(f"- {reason}" for reason in reasons)
    repository_card.extend(
        [
            "",
            "## Validation",
            "",
            specification["validation"],
            "",
            "The policy status deliberately distinguishes architecture-guided exclusions",
            "from exclusions proven necessary through individual ablation.",
            "",
        ]
    )
    (repository_root / "README.md").write_text("\n".join(repository_card))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True)
    parser.add_argument("--profile-config", required=True)
    parser.add_argument("--hf-path")
    parser.add_argument("--mlx-path")
    parser.add_argument("--q-bits", type=int)
    parser.add_argument("--q-group-size", type=int)
    parser.add_argument("--model-id")
    parser.add_argument("--source-repo")
    parser.add_argument("--source-revision")
    parser.add_argument("--source-license", default="apache-2.0")
    parser.add_argument("--annotate-only", action="store_true")
    parser.add_argument("--describe-profile", action="store_true")
    args = parser.parse_args()

    specification = load_profile(args.profile_config, args.profile)
    if args.describe_profile:
        print(json.dumps(specification, indent=2))
        return

    required = (args.mlx_path, args.model_id, args.source_repo, args.source_revision)
    if not all(required):
        parser.error("artifact annotation requires MLX path and source metadata")

    if not args.annotate_only:
        if args.hf_path is None or args.q_bits is None or args.q_group_size is None:
            parser.error("conversion requires HF path, q-bits, and q-group-size")
        apply_profile(args.profile, specification)

        from mlx_audio.convert import convert

        convert(
            hf_path=args.hf_path,
            mlx_path=args.mlx_path,
            quantize=True,
            q_bits=args.q_bits,
            q_group_size=args.q_group_size,
        )

    config_path = Path(args.mlx_path) / "config.json"
    config = json.loads(config_path.read_text())
    config["mica_quantization_profile"] = args.profile
    write_model_card(args, specification, config)
    config_path.write_text(json.dumps(dict(sorted(config.items())), indent=2) + "\n")


if __name__ == "__main__":
    main()

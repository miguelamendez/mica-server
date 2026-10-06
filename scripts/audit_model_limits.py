"""Read-only model metadata audit; developer tooling, never an inference dependency.

Run with: uv run --with pyyaml scripts/audit_model_limits.py --output REPORT.json
Architecture context is not proof of trained/useful context. Generation config
values are defaults, not hard supported/trained output limits.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import urllib.error
import urllib.request

import yaml


def fetch_json(repository, filename):
    url = f"https://huggingface.co/{repository}/raw/main/{filename}"
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "mica-metadata-audit"})
        with urllib.request.urlopen(request, timeout=20) as response:
            document = json.load(response)
        return {"url": url, "document": document}
    except (urllib.error.URLError, TimeoutError, ValueError) as error:
        return {"url": url, "error": str(error)}


def audit(manifest):
    model = yaml.safe_load(manifest.read_text())
    repository = model["source_repository"]
    config = fetch_json(repository, "config.json")
    generation = fetch_json(repository, "generation_config.json")
    document = config.pop("document", {})
    text = document.get("text_config", document)
    upstream_context = text.get("max_position_embeddings")
    causal = "text_generation" in model.get("abilities", [])
    declared = model.get("declared_context_tokens")
    defaults = generation.pop("document", {})
    if not causal:
        status = "not-a-causal-text-context-limit"
    elif upstream_context is None:
        status = "needs-model-card-or-artifact-verification"
    elif declared == upstream_context:
        status = "matches-architecture-config"
    else:
        status = "differs-from-architecture-config"
    return {
        "id": model["id"], "source_repository": repository,
        "declared_context_tokens": declared,
        "upstream_architecture_context_tokens": upstream_context,
        "status": status,
        "architecture": document.get("architectures"),
        "rope_scaling": text.get("rope_scaling", text.get("rope_parameters")),
        "trained_context_tokens": model.get("trained_context_tokens"),
        "useful_context_tokens": model.get("useful_context_tokens"),
        "supported_output_tokens": model.get("supported_output_tokens"),
        "trained_output_tokens": model.get("trained_output_tokens"),
        "generation_defaults_not_hard_limits": {
            key: defaults[key] for key in (
                "max_length", "max_new_tokens", "max_tokens", "do_sample",
                "temperature", "top_p", "top_k", "min_p",
                "repetition_penalty", "presence_penalty", "eos_token_id")
            if key in defaults
        },
        "config_source": config, "generation_source": generation,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-dir", type=Path,
                        default=Path(__file__).resolve().parent.parent / "config")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifests = sorted((args.config_dir / "model-manifests").glob("*.yaml"))
    if not manifests:
        parser.error("no model manifests found")
    with ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(audit, manifests))
    report = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "warning": "Upstream main is observed at audit time, not artifact certification. "
                   "Unknown training/output limits remain null; generation defaults "
                   "are not hard limits. No weights are downloaded or loaded.",
        "models": results,
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    for result in results:
        print(f"{result['id']}: {result['status']}; "
              f"manifest={result['declared_context_tokens']}, "
              f"upstream={result['upstream_architecture_context_tokens']}; "
              f"generation defaults={result['generation_defaults_not_hard_limits']}")
    if args.output:
        print(f"Report: {args.output}")


if __name__ == "__main__":
    main()

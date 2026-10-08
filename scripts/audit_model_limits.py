"""Read-only model metadata audit; developer tooling, never an inference dependency.

Run with: uv run --with pyyaml scripts/audit_model_limits.py --output REPORT.json
Architecture context is not proof of trained/recommended context. Generation config
values are defaults, not hard supported/trained output limits.
"""
import argparse
import hashlib
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
            content = response.read()
            document = json.loads(content)
        return {"url": url, "sha256": hashlib.sha256(content).hexdigest(),
                "document": document}
    except (urllib.error.URLError, TimeoutError, ValueError) as error:
        return {"url": url, "error": str(error)}


def audit(manifest):
    model = yaml.safe_load(manifest.read_text())
    repository = model["source_repository"]
    config = fetch_json(repository, "config.json")
    generation = fetch_json(repository, "generation_config.json")
    document = config.pop("document", {})
    text = document.get("text_config", document)
    upstream_context = text.get("max_position_embeddings", document.get("max_seq_len"))
    architecture_source = dict(config)
    # Quantized GGUF repositories may not ship an HF configuration. Use an
    # explicitly referenced base architecture, never an inferred model name.
    if upstream_context is None:
        for reference in model.get("references", []):
            prefix, suffix = "https://huggingface.co/", "/blob/main/config.json"
            url = reference["url"]
            if url.startswith(prefix) and url.endswith(suffix):
                base_repo = url[len(prefix):-len(suffix)]
                if base_repo == repository:
                    continue
                candidate = fetch_json(base_repo, "config.json")
                base = candidate.pop("document", {})
                base_text = base.get("text_config", base)
                limit = base_text.get("max_position_embeddings")
                if limit is not None:
                    upstream_context, text = limit, base_text
                    document = base
                    architecture_source = {**candidate, "kind": "referenced-base-architecture",
                                           "repository": base_repo}
                    break
    causal = "text_generation" in model.get("abilities", [])
    declared = model.get("native_context_tokens")
    defaults = generation.pop("document", {})
    if not causal and "speech_synthesis" not in model.get("abilities", []):
        status = "not-a-causal-text-context-limit"
    elif upstream_context is None:
        status = "needs-model-card-or-artifact-verification"
    elif declared == upstream_context:
        status = "matches-architecture-config"
    else:
        status = "differs-from-architecture-config"
    return {
        "id": model["id"], "source_repository": repository,
        "native_context_tokens": declared,
        "upstream_architecture_context_tokens": upstream_context,
        "status": status,
        "architecture": document.get("architectures"),
        "rope_scaling": text.get("rope_scaling", text.get("rope_parameters")),
        "recommended_context_tokens": model.get("recommended_context_tokens"),
        "max_output_tokens": model.get("max_output_tokens"),
        "context_units": "packed-text-audio-positions" if "speech_synthesis" in model.get("abilities", [])
                         else "tokens" if causal else "not-applicable",
        "references": model.get("references", []),
        "generation_defaults_not_hard_limits": {
            key: defaults[key] for key in (
                "max_length", "max_new_tokens", "max_tokens", "do_sample",
                "temperature", "top_p", "top_k", "min_p",
                "repetition_penalty", "presence_penalty", "eos_token_id")
            if key in defaults
        },
        "config_source": config, "architecture_source": architecture_source,
        "generation_source": generation,
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
                   "Unknown recommendations/output ceilings remain null; training disclosures "
                   "and publisher guidance are recorded in references. Generation defaults "
                   "are not hard limits. No weights are downloaded or loaded.",
        "models": results,
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    for result in results:
        print(f"{result['id']}: {result['status']}; "
              f"manifest={result['native_context_tokens']}, "
              f"upstream={result['upstream_architecture_context_tokens']}; "
              f"generation defaults={result['generation_defaults_not_hard_limits']}")
    if args.output:
        print(f"Report: {args.output}")


if __name__ == "__main__":
    main()

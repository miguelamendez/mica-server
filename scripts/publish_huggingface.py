#!/usr/bin/env python3
"""Publish validated Mica artifacts into one Hugging Face repo per model.

The script has no third-party Python dependency. It shells out to the isolated
`hf` executable installed by Mica, hashes every uploaded artifact, publishes
metadata first, and uploads one artifact at a time to keep disk and memory use
bounded. It never passes or prints a token; `hf` reads its normal token store.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_for(path: Path):
    if path.is_file():
        yield path, Path(path.name)
        return
    for item in sorted(path.rglob("*")):
        if (item.is_file() and ".cache" not in item.relative_to(path).parts and
                not item.name.startswith(".mica-complete-")):
            yield item, item.relative_to(path)


def run(command: list[str], dry_run: bool) -> None:
    print("+ " + " ".join(command))
    if not dry_run:
        subprocess.run(command, check=True)


def manifest_for(model_id: str, definition: dict, model_root: Path) -> dict:
    artifacts = []
    for artifact in definition["artifacts"]:
        if not artifact.get("validated", False):
            raise RuntimeError(f"refusing unvalidated artifact: {model_id}/{artifact['remote']}")
        local = model_root / artifact["local"]
        if not local.exists():
            raise FileNotFoundError(local)
        entries = []
        total = 0
        for item, relative in files_for(local):
            size = item.stat().st_size
            total += size
            entries.append({
                "path": relative.as_posix(),
                "size_bytes": size,
                "sha256": sha256(item),
            })
        artifacts.append({
            "backend": artifact["backend"],
            "quantization": artifact["quantization"],
            "path": artifact["remote"],
            "origin": artifact["origin"],
            "validated": True,
            "size_bytes": total,
            "files": entries,
        })
    return {
        "schema": 1,
        "model_id": model_id,
        "repository": definition["repository"],
        "created_at": datetime.now(timezone.utc).isoformat(),
        "publication_gate": "retained artifacts with real modality-specific inference",
        "vllm_artifacts_published": False,
        "artifacts": artifacts,
    }


def validation_remote(path: Path) -> str:
    if "benchmarks" in path.parts:
        return "validation/benchmarks/" + path.name
    return "validation/" + path.name


def publish_model(args: argparse.Namespace, model_id: str, definition: dict) -> None:
    repo_id = f"{args.namespace}/{definition['repository']}"
    manifest = manifest_for(model_id, definition, args.model_root)
    provenance = {
        "schema": 1,
        "model_id": model_id,
        "source": definition["source"],
        "artifact_origins": [
            {
                "backend": item["backend"],
                "quantization": item["quantization"],
                "path": item["remote"],
                "origin": item["origin"],
            }
            for item in definition["artifacts"]
        ],
        "excluded": {
            "vllm": "structural candidates were not quality-certified and are not published",
        },
    }

    with tempfile.TemporaryDirectory(prefix=f"mica-publish-{model_id}-") as temporary:
        staging = Path(temporary)
        (staging / "README.md").write_text(
            (args.project_root / definition["card"]).read_text()
        )
        (staging / "artifact-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        (staging / "source-provenance.json").write_text(
            json.dumps(provenance, indent=2) + "\n"
        )

        run([args.hf, "repo", "create", repo_id, "--type", "model", "--public", "--exist-ok"],
            args.dry_run)
        run([args.hf, "upload", repo_id, str(staging), ".",
             "--commit-message", "Add Mica model card and provenance"], args.dry_run)

    if args.metadata_only:
        return

    for source_name in definition.get("validation", []):
        source = args.project_root / source_name
        if not source.exists():
            raise FileNotFoundError(source)
        run([args.hf, "upload", repo_id, str(source), validation_remote(source),
             "--commit-message", "Add Mica validation evidence"], args.dry_run)

    for artifact in definition["artifacts"]:
        source = args.model_root / artifact["local"]
        run([args.hf, "upload", repo_id, str(source), artifact["remote"],
             "--commit-message",
             f"Add {artifact['backend']} {artifact['quantization']} artifact"], args.dry_run)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path,
                        default=PROJECT_ROOT / "config/huggingface_publish.json")
    parser.add_argument("--project-root", type=Path, default=PROJECT_ROOT)
    parser.add_argument("--model-root", type=Path, default=Path.home() / ".mica")
    parser.add_argument("--hf", default=str(Path.home() / ".mica/environments/tools/bin/hf"))
    parser.add_argument("--model", action="append",
                        help="Publish only this model id; may be repeated")
    parser.add_argument("--namespace")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--metadata-only", action="store_true",
                        help="Update card/manifests without validation or weight uploads")
    args = parser.parse_args()

    config = json.loads(args.config.read_text())
    args.namespace = args.namespace or config["namespace"]
    selected = args.model or list(config["models"])
    unknown = sorted(set(selected) - set(config["models"]))
    if unknown:
        parser.error("unknown model ids: " + ", ".join(unknown))
    for model_id in selected:
        publish_model(args, model_id, config["models"][model_id])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

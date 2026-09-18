#!/usr/bin/env python3
"""Verify uploaded Mica repository paths, sizes, and LFS SHA-256 values."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath

from huggingface_hub import HfApi


PROJECT_ROOT = Path(__file__).resolve().parent.parent


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path,
                        default=PROJECT_ROOT / "config/huggingface_publish.json")
    parser.add_argument("--model-root", type=Path, default=Path.home() / "models")
    parser.add_argument("--namespace")
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    namespace = args.namespace or config["namespace"]
    api = HfApi()
    report = {"schema": 1, "repositories": {}}
    failed = False
    for model_id, definition in config["models"].items():
        repo = f"{namespace}/{definition['repository']}"
        info = api.model_info(repo, files_metadata=True)
        remote = {item.rfilename: item for item in info.siblings}
        checks = []
        for required in ("README.md", "artifact-manifest.json", "source-provenance.json"):
            checks.append({"path": required, "present": required in remote})
            failed = failed or required not in remote
        for artifact in definition["artifacts"]:
            local = args.model_root / artifact["local"]
            files = [local] if local.is_file() else sorted(
                item for item in local.rglob("*")
                if (item.is_file() and ".cache" not in item.relative_to(local).parts and
                    not item.name.startswith(".mica-complete-"))
            )
            for item in files:
                relative = Path(item.name) if local.is_file() else item.relative_to(local)
                target = str(PurePosixPath(artifact["remote"]) / relative)
                if local.is_file():
                    target = artifact["remote"]
                sibling = remote.get(target)
                check = {
                    "path": target,
                    "present": sibling is not None,
                    "size_matches": sibling is not None and sibling.size == item.stat().st_size,
                    "sha256_matches": None,
                }
                if sibling is not None and sibling.lfs is not None:
                    check["sha256_matches"] = sibling.lfs.sha256 == digest(item)
                failed = failed or not check["present"] or not check["size_matches"] or \
                    check["sha256_matches"] is False
                checks.append(check)
        report["repositories"][repo] = {
            "revision": info.sha,
            "checks": checks,
            "passed": all(item["present"] and item.get("size_matches", True) and
                          item.get("sha256_matches") is not False for item in checks),
        }
    report["passed"] = not failed
    print(json.dumps(report, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

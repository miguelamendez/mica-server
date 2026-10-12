"""Offline maintainer packaging: sync reference assets and refresh checksums.

uv run --with pyyaml python scripts/package_demo_apps.py
This mechanical packaging tool is not a native runtime dependency.
"""
import hashlib
from pathlib import Path
import shutil
import yaml


def main():
    root = Path(__file__).resolve().parent.parent
    apps = root / "apps"
    for folder in ("assistant", "dictation"):
        runtime = apps / folder / "runtime"
        runtime.mkdir(exist_ok=True)
        shutil.copyfile(apps / "common/mcp_runtime.py", runtime / "mcp_runtime.py")
    for name in ("mica_playground.py", "mica-logo.svg", "mica-capabilities.js", "mica-chat-settings.js"):
        shutil.copyfile(apps / name, apps / "assistant/runtime" / name)
    for source, target in (("mac_assistant", "mac"), ("gpu_16g_assistant", "cuda")):
        shutil.copyfile(root / "config/workloads" / f"{source}.yaml", apps / "assistant/workloads" / f"{target}.yaml")
    index = yaml.safe_load((apps / "apps.yaml").read_text())
    for item in index["apps"]:
        path = apps / item["manifest"]
        document = yaml.safe_load(path.read_text())
        item["name"] = document["name"]
        item["description"] = document["description"]
        files = []
        for payload in sorted(path.parent.rglob("*")):
            if not payload.is_file() or payload == path or "__pycache__" in payload.parts or "build" in payload.parts or payload.suffix == ".pyc":
                continue
            data = payload.read_bytes()
            files.append({"path": payload.relative_to(path.parent).as_posix(),
                          "sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data)})
        document["distribution"]["files"] = files
        path.write_text(yaml.safe_dump(document, sort_keys=False))
        item["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    (apps / "apps.yaml").write_text(yaml.safe_dump(index, sort_keys=False))
    print("Synchronized assistant assets and refreshed demo file/manifest SHA-256 digests.")


if __name__ == "__main__":
    main()

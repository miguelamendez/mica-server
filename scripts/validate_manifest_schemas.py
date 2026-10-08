"""Offline schema checks (uv run --with pyyaml --with jsonschema ...).

These dependencies are developer tools, not native Mica runtime requirements.
"""
import json
import re
from pathlib import Path

import yaml
from jsonschema import Draft202012Validator
from referencing import Registry, Resource


class Yaml12Loader(yaml.SafeLoader):
    pass


# PyYAML defaults to YAML 1.1 and misreads CMake ON/OFF as booleans. Match
# Mica's YAML 1.2-like scalar rules without modifying the global SafeLoader.
Yaml12Loader.yaml_implicit_resolvers = {
    key: [(tag, pattern) for tag, pattern in resolvers if tag != "tag:yaml.org,2002:bool"]
    for key, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
Yaml12Loader.add_implicit_resolver("tag:yaml.org,2002:bool",
    re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"), list("tTfF"))


def validate_model_limits(document):
    """JSON Schema types first; enforce scalar relationships like the loader."""
    native = document.get("native_context_tokens")
    if "text_generation" in document.get("abilities", []) and native is None:
        raise ValueError("text-generation model needs native_context_tokens")
    for field in ("recommended_context_tokens", "max_output_tokens"):
        value = document.get(field)
        if value is not None and (native is None or value > native):
            raise ValueError(f"{field} exceeds native_context_tokens")


def main():
    root = Path(__file__).resolve().parent.parent
    schemas = {p.name: json.loads(p.read_text()) for p in (root / "schemas").glob("*.json")}
    registry = Registry().with_resources(
        (s["$id"], Resource.from_contents(s)) for s in schemas.values() if "$id" in s
    )
    count = 0
    for folder, name in (("engines", "engine-v2.schema.json"),
                         ("model-manifests", "model-v2.schema.json"),
                         ("workloads", "workload-v5.schema.json")):
        validator = Draft202012Validator(schemas[name], registry=registry)
        for path in sorted((root / "config" / folder).glob("*.yaml")):
            document = yaml.load(path.read_text(), Loader=Yaml12Loader)
            errors = list(validator.iter_errors(document))
            if errors:
                raise ValueError(f"{path.relative_to(root)}: {errors[0].message}")
            if folder == "model-manifests":
                validate_model_limits(document)
            count += 1
    validator = Draft202012Validator(schemas["workload-v5.schema.json"], registry=registry)
    proposals = 0
    for document in yaml.load((root / "profiles/catalog.yaml").read_text(), Loader=Yaml12Loader)["profiles"]:
        if document.get("available") is False and "models" not in document:
            assert document.get("description") and document.get("target_models")
            proposals += 1
            continue  # Explicitly non-installable catalog proposal, not a workload YAML.
        # Catalog-only annotations are not executable workload policy fields.
        document = {key: value for key, value in document.items() if key not in {"reason", "status"}}
        validator.validate(document)
        count += 1
    print(f"Validated {count} engine, model, workload, and catalog documents against their schemas; "
          f"{proposals} non-installable catalog proposals excluded.")


if __name__ == "__main__":
    main()

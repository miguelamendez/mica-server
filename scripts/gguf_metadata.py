#!/usr/bin/env python3
"""Inspect GGUF metadata without numpy or loading model tensor payloads.

Also works on a downloaded first range containing the complete header. This
is an offline inspection tool; it is not part of Mica's inference runtime.
"""
import argparse
import json
from pathlib import Path
import struct

SCALARS = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}


def inspect(path):
    with Path(path).open("rb") as stream:
        def scalar(fmt):
            return struct.unpack("<" + fmt, stream.read(struct.calcsize(fmt)))[0]

        def string(keep):
            size = scalar("Q")
            if size > 64 * 1024 * 1024:
                raise ValueError("unreasonable GGUF string size")
            if keep:
                return stream.read(size).decode("utf-8")
            stream.seek(size, 1)

        def value(kind, keep):
            if kind in SCALARS:
                return scalar(SCALARS[kind])
            if kind == 8:
                return string(keep)
            if kind == 9:
                subtype, count = scalar("I"), scalar("Q")
                if count > 10000000:
                    raise ValueError("unreasonable GGUF array size")
                if not keep and subtype in SCALARS:
                    stream.seek(count * struct.calcsize(SCALARS[subtype]), 1)
                    return None
                items = []
                for _ in range(count):
                    item = value(subtype, keep)
                    if keep:
                        items.append(item)
                return items if keep else None
            raise ValueError(f"unsupported GGUF metadata type: {kind}")

        if stream.read(4) != b"GGUF":
            raise ValueError("not a GGUF file")
        version = scalar("I")
        if version not in (2, 3):
            raise ValueError("unsupported GGUF version")
        tensors, count = scalar("Q"), scalar("Q")
        metadata = {}
        for _ in range(count):
            key = string(True)
            kind = scalar("I")
            keep = not key.startswith("tokenizer.")
            item = value(kind, keep)
            if keep:
                metadata[key] = item
        return {"version": version, "tensor_count": tensors, "metadata": metadata}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    print(json.dumps(inspect(parser.parse_args().path), indent=2))

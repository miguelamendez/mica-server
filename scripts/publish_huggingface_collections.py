#!/usr/bin/env python3
"""Create/update Mica's Hugging Face collections grouped by modality."""

from __future__ import annotations

import argparse

from huggingface_hub import HfApi


COLLECTIONS = {
    "Mica Local Text Models": {
        "description": "Permissively licensed local text-generation artifacts validated by Mica.",
        "models": ["mica-spark-x25-4b"],
    },
    "Mica Local Speech Recognition": {
        "description": "Permissively licensed local ASR artifacts validated by Mica.",
        "models": ["mica-granite-speech-5"],
    },
    "Mica Local Text to Speech": {
        "description": "Permissively licensed local TTS and voice-cloning artifacts validated by Mica.",
        "models": ["mica-audio8-tts-06b"],
    },
    "Mica Local Multimodal Models": {
        "description": "Permissively licensed local image/video-to-text artifacts validated by Mica.",
        "models": ["mica-minicpm-v46-thinking"],
    },
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--namespace", default="miguelamendez")
    parser.add_argument("--private", action="store_true")
    args = parser.parse_args()
    api = HfApi()
    for title, definition in COLLECTIONS.items():
        collection = api.create_collection(
            title, namespace=args.namespace, description=definition["description"],
            private=args.private, exists_ok=True,
        )
        for model in definition["models"]:
            api.add_collection_item(
                collection.slug, f"{args.namespace}/{model}", "model", exists_ok=True,
            )
        print(f"https://huggingface.co/collections/{collection.slug}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

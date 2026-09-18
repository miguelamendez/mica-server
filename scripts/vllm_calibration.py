#!/usr/bin/env python3
"""Modality-aware calibration inputs for compressed-tensors candidates.

Non-text calibration is driven by a local JSONL manifest so every sample and
license can be audited before a candidate is published.  Records use paths
relative to the manifest unless an absolute path is supplied.
"""

from __future__ import annotations

import hashlib
import json
import wave
from pathlib import Path
from typing import Any


SCHEMA = 1


def _resolved(base: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (base / path).resolve()


def load_manifest(path: Path, capability: str, limit: int) -> tuple[list[dict], dict]:
    if not path.is_file():
        raise ValueError(f"calibration manifest does not exist: {path}")
    records = []
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as error:
            raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
        if record.get("schema", SCHEMA) != SCHEMA:
            raise ValueError(f"{path}:{line_number}: unsupported record schema")
        record["_line"] = line_number
        records.append(record)
    if len(records) < limit:
        raise ValueError(
            f"calibration manifest has {len(records)} records but {limit} were requested"
        )
    records = records[:limit]
    mix: dict[str, int] = {}
    base = path.parent
    for record in records:
        line = record["_line"]
        if capability == "vision":
            modality = record.get("modality")
            if modality not in {"image", "video"}:
                raise ValueError(f"{path}:{line}: vision modality must be image or video")
            if not str(record.get("prompt", "")).strip():
                raise ValueError(f"{path}:{line}: vision prompt is required")
            fields = [record.get("media")] if modality == "image" else record.get("frames", [])
            if not fields or any(not _resolved(base, item).is_file() for item in fields):
                raise ValueError(f"{path}:{line}: vision media files are missing")
            mix[modality] = mix.get(modality, 0) + 1
        elif capability == "asr":
            if not record.get("audio") or not _resolved(base, record["audio"]).is_file():
                raise ValueError(f"{path}:{line}: ASR audio file is missing")
            mix["audio"] = mix.get("audio", 0) + 1
        elif capability == "tts":
            if not str(record.get("text", "")).strip():
                raise ValueError(f"{path}:{line}: TTS text is required")
            has_audio = bool(record.get("reference_audio"))
            has_codes = bool(record.get("reference_codes"))
            if has_audio and has_codes:
                raise ValueError(f"{path}:{line}: choose reference_audio or reference_codes")
            if has_audio and not _resolved(base, record["reference_audio"]).is_file():
                raise ValueError(f"{path}:{line}: TTS reference audio is missing")
            if has_codes and not _resolved(base, record["reference_codes"]).is_file():
                raise ValueError(f"{path}:{line}: TTS reference codes are missing")
            if (has_audio or has_codes) and not str(record.get("reference_text", "")).strip():
                raise ValueError(f"{path}:{line}: reference_text is required for voice cloning")
            key = "reference" if has_audio or has_codes else "plain"
            mix[key] = mix.get(key, 0) + 1
        else:
            raise ValueError(f"no manifest adapter for capability: {capability}")
    required = {
        "vision": {"image", "video"},
        "asr": {"audio"},
        "tts": {"plain", "reference"},
    }[capability]
    missing = sorted(required - set(mix))
    if missing:
        raise ValueError(
            f"calibration manifest lacks required {capability} sample types: {', '.join(missing)}"
        )
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return records, {
        "manifest": str(path.resolve()),
        "manifest_sha256": digest,
        "records_used": len(records),
        "sample_mix": mix,
    }


def _read_audio(path: Path) -> tuple[Any, int]:
    try:
        import soundfile

        samples, rate = soundfile.read(path, dtype="float32", always_2d=True)
        return samples.mean(axis=1), int(rate)
    except ImportError:
        pass
    with wave.open(str(path), "rb") as handle:
        if handle.getsampwidth() != 2:
            raise ValueError("stdlib WAV fallback requires signed 16-bit PCM")
        import numpy

        channels = handle.getnchannels()
        rate = handle.getframerate()
        values = numpy.frombuffer(handle.readframes(handle.getnframes()), dtype="<i2")
        values = values.reshape(-1, channels).astype("float32").mean(axis=1) / 32768.0
        return values, int(rate)


class _ManifestDataset:
    def __init__(self, records: list[dict], base: Path, capability: str,
                 processor: Any, model: Any, max_length: int):
        self.records = records
        self.base = base
        self.capability = capability
        self.processor = processor
        self.model = model
        self.max_length = max_length

    def __len__(self) -> int:
        return len(self.records)

    def __getitem__(self, index: int) -> dict:
        record = self.records[index]
        if self.capability == "vision":
            return self._vision(record)
        if self.capability == "asr":
            return self._asr(record)
        if self.capability == "tts":
            return self._tts(record)
        raise AssertionError(self.capability)

    def _vision(self, record: dict) -> dict:
        from PIL import Image

        modality = record["modality"]
        content = [{"type": modality}, {"type": "text", "text": record["prompt"]}]
        rendered = self.processor.apply_chat_template(
            [{"role": "user", "content": content}],
            tokenize=False,
            add_generation_prompt=True,
        )
        common = {
            "text": [rendered],
            "return_tensors": "pt",
            # Transformers <=5.14 has a NaViT reshape bug when rectangular
            # inputs create unequal MiniCPM slice grids. Square/padded
            # calibration or one-slice preprocessing avoids corrupting the
            # calibration trace while retaining genuine vision embeddings.
            "max_slice_nums": int(record.get("max_slice_nums", 1)),
        }
        if modality == "image":
            image = Image.open(_resolved(self.base, record["media"])).convert("RGB")
            values = self.processor(images=[image], **common)
        else:
            frames = [
                Image.open(_resolved(self.base, value)).convert("RGB")
                for value in record["frames"]
            ]
            values = self.processor(videos=[frames], do_sample_frames=False, **common)
        input_ids = values["input_ids"]
        if input_ids.shape[-1] > self.max_length:
            raise ValueError(
                f"multimodal sample uses {input_ids.shape[-1]} tokens, above the "
                f"calibration limit {self.max_length}; reduce media frames/resolution"
            )

        # LLM Compressor's FX tracer cannot currently preserve MiniCPM's
        # NaViT-packed pixel layout. Run the protected vision tower/merger first
        # and feed the resulting real multimodal embeddings to the language
        # model being calibrated.
        import torch

        with torch.inference_mode():
            inputs_embeds = self.model.get_input_embeddings()(input_ids.to(self.model.device))
            if modality == "image":
                vision = self.model.get_image_features(
                    values["pixel_values"].to(self.model.device),
                    values["target_sizes"].to(self.model.device),
                )
                features = torch.cat(vision.pooler_output, dim=0).to(
                    device=inputs_embeds.device, dtype=inputs_embeds.dtype
                )
                token_id = self.model.config.image_token_id
            else:
                vision = self.model.get_video_features(
                    values["pixel_values_videos"].to(self.model.device),
                    values["target_sizes_videos"].to(self.model.device),
                )
                features = torch.cat(vision.pooler_output, dim=0).to(
                    device=inputs_embeds.device, dtype=inputs_embeds.dtype
                )
                token_id = self.model.config.video_token_id
            mask = self.model.model.get_placeholder_mask(
                input_ids.to(self.model.device), inputs_embeds, features, token_id
            )
            inputs_embeds = inputs_embeds.masked_scatter(mask, features)
        return {
            "inputs_embeds": inputs_embeds.detach().cpu(),
            "attention_mask": values["attention_mask"].detach().cpu(),
        }

    def _asr(self, record: dict) -> dict:
        samples, rate = _read_audio(_resolved(self.base, record["audio"]))
        if hasattr(self.processor, "feature_extractor"):
            expected = int(self.processor.feature_extractor.sampling_rate)
        else:
            expected = int(self.processor.sample_rate)
        declared = int(record.get("sampling_rate", rate))
        if declared != rate:
            raise ValueError(f"declared sampling rate {declared} does not match file rate {rate}")
        if rate != expected:
            raise ValueError(f"ASR sample is {rate} Hz; model requires {expected} Hz")
        if hasattr(self.processor, "feature_extractor"):
            values = self.processor(
                audio=[samples], sampling_rate=rate, return_tensors="pt", padding=True
            )
        else:
            values = self.processor([samples], sampling_rate=rate, device="cpu")
        return {key: value for key, value in values.items() if key != "labels"}

    def _tts(self, record: dict) -> dict:
        kwargs: dict[str, Any] = {"text": [record["text"]], "return_tensors": "pt"}
        if record.get("reference_audio"):
            samples, rate = _read_audio(_resolved(self.base, record["reference_audio"]))
            kwargs.update(
                reference_text=[record["reference_text"]],
                reference_audio=[samples],
                sampling_rate=[rate],
            )
        elif record.get("reference_codes"):
            import numpy

            kwargs.update(
                reference_text=[record["reference_text"]],
                reference_codes=[numpy.load(_resolved(self.base, record["reference_codes"]))],
            )
        processor_values = self.processor(**kwargs)
        import torch

        with torch.inference_mode():
            input_ids, attention_mask = self.model._prepare_prompt(**processor_values)
        if input_ids.shape[-1] > self.max_length:
            input_ids = input_ids[..., -self.max_length :]
            attention_mask = attention_mask[..., -self.max_length :]
        return {
            "input_ids": input_ids.detach().cpu(),
            "attention_mask": attention_mask.detach().cpu(),
        }


def build_manifest_dataloader(path: Path, capability: str, processor: Any,
                              model: Any, samples: int, max_length: int,
                              batch_size: int):
    if batch_size != 1:
        raise ValueError(
            f"{capability} calibration currently requires batch_size=1 to avoid "
            "padding media activations into misleading calibration samples"
        )
    records, metadata = load_manifest(path, capability, samples)
    from torch.utils.data import DataLoader

    dataset = _ManifestDataset(records, path.parent, capability, processor, model, max_length)
    return DataLoader(dataset, batch_size=None, num_workers=0), metadata


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Validate a Mica vLLM calibration manifest")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--capability", choices=("vision", "asr", "tts"), required=True)
    parser.add_argument("--samples", type=int, required=True)
    args = parser.parse_args()
    _, metadata = load_manifest(args.manifest, args.capability, args.samples)
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

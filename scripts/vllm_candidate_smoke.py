#!/usr/bin/env python3
"""Run real Transformers inference on a compressed-tensors candidate."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--processor", type=Path)
    parser.add_argument("--prompt", default="")
    parser.add_argument("--max-new-tokens", type=int, default=32)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--capability", choices=("text", "vision", "asr", "tts"), default="text")
    parser.add_argument("--image", type=Path)
    parser.add_argument("--video-frames", type=Path, nargs="+")
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--reference-audio", type=Path)
    parser.add_argument("--reference-text")
    parser.add_argument("--audio-output", type=Path)
    args = parser.parse_args()
    if args.batch_size < 1:
        parser.error("--batch-size must be positive")

    import torch
    from transformers import (
        AutoModelForCausalLM,
        AutoModel,
        AutoModelForCTC,
        AutoModelForImageTextToText,
        AutoProcessor,
        AutoTokenizer,
    )

    tokenizer = AutoTokenizer.from_pretrained(args.model, trust_remote_code=True)
    loaders = {
        "text": AutoModelForCausalLM,
        "vision": AutoModelForImageTextToText,
        "asr": AutoModelForCTC,
        "tts": AutoModel,
    }
    loader = loaders[args.capability]
    model = loader.from_pretrained(
        args.model,
        trust_remote_code=True,
        dtype="auto",
        device_map=args.device,
        low_cpu_mem_usage=True,
    )
    processor = None
    media_kind = "text"
    if args.capability == "vision" and (args.image or args.video_frames):
        from PIL import Image

        processor = AutoProcessor.from_pretrained(
            args.processor or args.model, trust_remote_code=True
        )
        if args.image and args.video_frames:
            parser.error("choose --image or --video-frames")
        if args.image:
            media_kind = "image"
            content = [{"type": "image"}, {"type": "text", "text": args.prompt}]
            rendered = processor.apply_chat_template(
                [{"role": "user", "content": content}],
                tokenize=False, add_generation_prompt=True,
            )
            inputs = processor(
                text=[rendered] * args.batch_size,
                images=[Image.open(args.image).convert("RGB")] * args.batch_size,
                max_slice_nums=1, return_tensors="pt",
            ).to(args.device)
        else:
            media_kind = "video"
            content = [{"type": "video"}, {"type": "text", "text": args.prompt}]
            rendered = processor.apply_chat_template(
                [{"role": "user", "content": content}],
                tokenize=False, add_generation_prompt=True,
            )
            frames = [Image.open(path).convert("RGB") for path in args.video_frames]
            inputs = processor(
                text=[rendered] * args.batch_size,
                videos=[frames] * args.batch_size, do_sample_frames=False,
                max_slice_nums=1, return_tensors="pt",
            ).to(args.device)
    elif args.capability == "text" or args.capability == "vision":
        messages = [{"role": "user", "content": args.prompt}]
        rendered = tokenizer.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True
        )
        inputs = tokenizer(
            [rendered] * args.batch_size, padding=True, return_tensors="pt"
        ).to(args.device)
    elif args.capability == "asr":
        if not args.audio:
            parser.error("ASR smoke requires --audio")
        import soundfile

        samples, rate = soundfile.read(args.audio, dtype="float32", always_2d=True)
        processor = AutoProcessor.from_pretrained(
            args.processor or args.model, trust_remote_code=True
        )
        inputs = processor(
            audio=[samples.mean(axis=1)] * args.batch_size, sampling_rate=rate,
            return_tensors="pt", padding=True,
        ).to(args.device)
        start = time.perf_counter()
        with torch.inference_mode():
            output = model(**inputs)
        elapsed = time.perf_counter() - start
        token_ids = output.logits.argmax(dim=-1)
        completions = processor.batch_decode(token_ids)
        result = {
            "transcripts": completions,
            "audio_seconds": len(samples) / rate,
            "realtime_factor_per_stream": elapsed / (len(samples) / rate),
            "aggregate_realtime_factor": elapsed / (
                args.batch_size * len(samples) / rate
            ),
        }
    else:
        if not args.audio_output:
            parser.error("TTS smoke requires --audio-output")
        import soundfile

        processor = AutoProcessor.from_pretrained(
            args.processor or args.model, trust_remote_code=True
        )
        processor_args = {
            "text": [args.prompt] * args.batch_size,
            "return_tensors": "pt",
        }
        if args.reference_audio:
            if not args.reference_text:
                parser.error("--reference-text is required with --reference-audio")
            samples, rate = soundfile.read(
                args.reference_audio, dtype="float32", always_2d=True
            )
            processor_args.update(
                reference_text=[args.reference_text] * args.batch_size,
                reference_audio=[samples.mean(axis=1)] * args.batch_size,
                sampling_rate=[rate] * args.batch_size,
            )
        inputs = processor(**processor_args).to(args.device)
        # ArkTTS generation calls private slow/fast decode methods directly,
        # bypassing the model-level forward pre-hook installed by
        # compressed-tensors. One ordinary forward activates decompression for
        # this Transformers-only smoke path; vLLM-Omni has its own loader.
        with torch.inference_mode():
            prompt_ids, prompt_mask = model._prepare_prompt(**inputs)
            model(input_ids=prompt_ids, attention_mask=prompt_mask)
        start = time.perf_counter()
        with torch.inference_mode():
            waveforms, lengths, codes = model.generate_audio(
                **inputs, max_new_tokens=args.max_new_tokens
            )
        elapsed = time.perf_counter() - start
        sample_rate = int(getattr(processor, "audio_sampling_rate", 44100))
        args.audio_output.parent.mkdir(parents=True, exist_ok=True)
        audio_outputs = []
        audio_seconds = []
        for index, raw_length in enumerate(lengths):
            length = int(raw_length)
            output_path = (
                args.audio_output
                if args.batch_size == 1
                else args.audio_output.with_stem(f"{args.audio_output.stem}-{index}")
            )
            soundfile.write(
                output_path,
                waveforms[index, :length].float().cpu().numpy(),
                sample_rate,
            )
            audio_outputs.append(str(output_path))
            audio_seconds.append(length / sample_rate)
        result = {
            "audio_outputs": audio_outputs,
            "audio_seconds": audio_seconds,
            "realtime_factor_per_stream": elapsed / min(audio_seconds),
            "aggregate_realtime_factor": elapsed / sum(audio_seconds),
            "generated_code_frames": int(codes.shape[-1]),
            "voice_reference": bool(args.reference_audio),
        }

    if args.capability in {"text", "vision"}:
        start = time.perf_counter()
        with torch.inference_mode():
            output = model.generate(
                **inputs,
                do_sample=False,
                max_new_tokens=args.max_new_tokens,
            )
        elapsed = time.perf_counter() - start
        input_length = inputs["input_ids"].shape[1]
        completions = tokenizer.batch_decode(
            output[:, input_length:], skip_special_tokens=True
        )
        result = {
            "completions": completions,
            "completion_tokens_per_stream": int(output.shape[1] - input_length),
        }
    payload = {
        "model": str(args.model),
        "device": args.device,
        "prompt": args.prompt,
        "batch_size": args.batch_size,
        "media_kind": media_kind,
        "elapsed_seconds": elapsed,
        **result,
    }
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

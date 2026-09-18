#!/usr/bin/env python3
"""Benchmark lossless classic speculative decoding for MLX-VLM text models.

MLX-VLM currently exposes specialized DFlash/EAGLE/MTP drafters, while
MLX-LM has the ordinary smaller-peer speculative loop.  This script adapts an
MLX-VLM language model to that loop and compares its greedy token sequence to
plain greedy decoding before reporting any speed result.
"""

from __future__ import annotations

import argparse
import gc
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import mlx.core as mx
import mlx.nn as nn
from mlx_lm.generate import generate_step, speculative_generate_step
from mlx_vlm.utils import load


class LogitsAdapter(nn.Module):
    """Return raw logits while preserving MLX-VLM's model/cache contracts."""

    def __init__(self, model: nn.Module):
        super().__init__()
        self.model = getattr(model, "language_model", model)

    def __call__(self, inputs, cache=None):
        output = self.model(inputs, cache=cache)
        return getattr(output, "logits", output)

    @property
    def layers(self):
        return self.model.layers

    def make_cache(self):
        return self.model.make_cache()


@dataclass
class Run:
    elapsed_seconds: float
    completion_tokens: int
    tokens_per_second: float
    peak_memory_gib: float
    accepted_draft_tokens: int = 0
    target_verified_tokens: int = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--draft", type=Path, required=True)
    parser.add_argument(
        "--prompt",
        default=(
            "Explain in three short paragraphs how speculative decoding works, "
            "including why target-model verification preserves correctness."
        ),
    )
    parser.add_argument("--max-tokens", type=int, default=192)
    parser.add_argument("--num-draft-tokens", type=int, default=2)
    parser.add_argument(
        "--trust-remote-code",
        action="store_true",
        help="Allow processor/tokenizer code shipped with the local checkpoint.",
    )
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def eos_ids(tokenizer) -> set[int]:
    value = getattr(tokenizer, "eos_token_id", None)
    if value is None:
        value = getattr(tokenizer, "eos_token_ids", ())
    if value is None:
        return set()
    if isinstance(value, int):
        return {value}
    return set(value)


def completion(
    rows: Iterable[tuple], stop_ids: set[int], max_tokens: int
) -> tuple[list[int], int, int]:
    tokens: list[int] = []
    accepted = 0
    verified = 0
    for row in rows:
        token = int(row[0].item() if hasattr(row[0], "item") else row[0])
        from_draft = bool(row[2]) if len(row) > 2 else False
        tokens.append(token)
        accepted += int(from_draft)
        verified += int(not from_draft)
        if token in stop_ids or len(tokens) >= max_tokens:
            break
    return tokens, accepted, verified


def timed_run(factory, stop_ids: set[int], max_tokens: int) -> tuple[list[int], Run]:
    gc.collect()
    mx.clear_cache()
    if hasattr(mx, "reset_peak_memory"):
        mx.reset_peak_memory()
    started = time.perf_counter()
    tokens, accepted, verified = completion(factory(), stop_ids, max_tokens)
    mx.synchronize()
    elapsed = time.perf_counter() - started
    peak = mx.get_peak_memory() if hasattr(mx, "get_peak_memory") else 0
    return tokens, Run(
        elapsed_seconds=elapsed,
        completion_tokens=len(tokens),
        tokens_per_second=(len(tokens) / elapsed if elapsed else 0.0),
        peak_memory_gib=peak / (1024**3),
        accepted_draft_tokens=accepted,
        target_verified_tokens=verified,
    )


def main() -> int:
    args = parse_args()
    if args.max_tokens < 1 or args.num_draft_tokens < 1:
        raise SystemExit("--max-tokens and --num-draft-tokens must be positive")

    target, processor = load(
        str(args.target), trust_remote_code=args.trust_remote_code
    )
    draft, draft_processor = load(
        str(args.draft), trust_remote_code=args.trust_remote_code
    )
    tokenizer = getattr(processor, "tokenizer", processor)
    draft_tokenizer = getattr(draft_processor, "tokenizer", draft_processor)

    target_vocab = tokenizer.get_vocab()
    draft_vocab = draft_tokenizer.get_vocab()
    if target_vocab != draft_vocab:
        raise SystemExit("target and draft tokenizers are not identical")

    formatted = tokenizer.apply_chat_template(
        [{"role": "user", "content": args.prompt}],
        tokenize=False,
        add_generation_prompt=True,
    )
    prompt = mx.array(tokenizer.encode(formatted, add_special_tokens=False))
    target_model = LogitsAdapter(target)
    draft_model = LogitsAdapter(draft)
    stops = eos_ids(tokenizer)

    # Materialize both checkpoints before either timed run.
    mx.eval(target_model.parameters(), draft_model.parameters())

    plain_tokens, plain = timed_run(
        lambda: generate_step(prompt, target_model, max_tokens=args.max_tokens),
        stops,
        args.max_tokens,
    )
    speculative_tokens, speculative = timed_run(
        lambda: speculative_generate_step(
            prompt,
            target_model,
            draft_model,
            max_tokens=args.max_tokens,
            num_draft_tokens=args.num_draft_tokens,
        ),
        stops,
        args.max_tokens,
    )

    result = {
        "target": str(args.target),
        "draft": str(args.draft),
        "prompt": args.prompt,
        "prompt_tokens": int(prompt.size),
        "num_draft_tokens": args.num_draft_tokens,
        "token_sequences_equal": plain_tokens == speculative_tokens,
        "plain": asdict(plain),
        "speculative": asdict(speculative),
        "speculative_output_token_share": (
            speculative.accepted_draft_tokens / speculative.completion_tokens
            if speculative.completion_tokens
            else 0.0
        ),
        "speedup": (
            speculative.tokens_per_second / plain.tokens_per_second
            if plain.tokens_per_second
            else 0.0
        ),
        "plain_text": tokenizer.decode(plain_tokens, skip_special_tokens=True),
        "speculative_text": tokenizer.decode(
            speculative_tokens, skip_special_tokens=True
        ),
    }
    rendered = json.dumps(result, indent=2, ensure_ascii=False)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0 if result["token_sequences_equal"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

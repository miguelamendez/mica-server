#!/usr/bin/env python3
"""Run a reproducible multi-task/long-context classic-speculation audit."""

from __future__ import annotations

import argparse
import gc
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import mlx.core as mx
from mlx_lm.generate import stream_generate
from mlx_vlm.utils import load

from mlx_classic_speculative import LogitsAdapter


SUMMARY_LENGTHS = (512, 1024, 2048, 4096, 8192, 16384)


@dataclass
class Measurement:
    wall_seconds: float
    prompt_tokens: int
    prompt_tokens_per_second: float
    time_to_first_token_seconds: float
    completion_tokens: int
    decode_tokens_per_second: float
    peak_memory_gb: float
    accepted_draft_tokens: int
    target_verified_tokens: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--draft", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--num-draft-tokens", type=int, default=2)
    parser.add_argument("--trust-remote-code", action="store_true")
    return parser.parse_args()


def task_cases() -> list[dict]:
    return [
        {
            "id": "logic-reasoning",
            "task": "logic",
            "max_tokens": 768,
            "content": (
                "Solve this logic problem rigorously. Eight researchers—Ada, Ben, "
                "Chen, Dia, Eli, Fara, Gus, and Hana—present exactly once in slots "
                "1 through 8. Ada is earlier than Chen but later than Fara. Ben is "
                "immediately before neither Dia nor Eli. Gus is exactly three slots "
                "after Chen. Hana is in an even slot and before Dia. Eli is adjacent "
                "to Fara. Chen is not in slot 3. Determine every valid schedule. "
                "Show a complete constraint-propagation argument, enumerate branches, "
                "verify every solution, and explain why no other schedule is possible. "
                "Use at least 600 tokens."
            ),
        },
        {
            "id": "coding",
            "task": "coding",
            "max_tokens": 1024,
            "content": (
                "Write a complete Python 3.12 implementation of a thread-safe, "
                "capacity-bounded LRU cache with per-entry TTL, deterministic eviction, "
                "a get-or-compute operation that prevents duplicate concurrent work, "
                "and statistics. Include type hints, docstrings, careful exception "
                "handling, and pytest tests for expiration, concurrency, eviction, and "
                "failed computations. Then explain the synchronization invariants and "
                "complexity. Produce a substantial implementation, not pseudocode."
            ),
        },
        {
            "id": "creative-writing",
            "task": "creativity",
            "max_tokens": 1024,
            "content": (
                "Write a polished literary science-fiction story of at least 1,200 "
                "words about a lighthouse keeper whose light warns ships about events "
                "that have not happened yet. Use vivid sensory detail, distinct "
                "dialogue, a clear emotional arc, and an ending that reinterprets an "
                "earlier image. Do not outline or discuss the request; write the story."
            ),
        },
        {
            "id": "general-knowledge",
            "task": "general-knowledge",
            "max_tokens": 768,
            "content": (
                "Write an accurate comparative essay of at least 800 words explaining "
                "how the printing press, telegraph, radio, and internet each changed "
                "the speed, cost, gatekeeping, and social reach of information. Separate "
                "well-established historical facts from interpretation, identify two "
                "misleading analogies between these transitions, and finish with a "
                "compact comparison table."
            ),
        },
        {
            "id": "structured-json",
            "task": "structured-output",
            "max_tokens": 1024,
            "content": (
                "Return only valid JSON. Create an object with a `schema_version` of 1 "
                "and an `experiments` array containing exactly 60 distinct records. "
                "Every record must contain integer `id`, string `category`, numeric "
                "`score`, boolean `passed`, and a three-element string array `tags`. "
                "Alternate four categories deterministically, make scores reproducible, "
                "and include no Markdown or comments."
            ),
        },
    ]


def synthetic_document() -> str:
    topics = (
        "water allocation", "rail maintenance", "hospital staffing",
        "coastal restoration", "school transport", "grid storage",
        "housing permits", "wildfire readiness", "port logistics",
        "public libraries", "agricultural research", "air-quality monitoring",
    )
    outcomes = (
        "reduced delays", "raised operating costs", "improved reliability",
        "shifted demand", "exposed a reporting gap", "lowered emissions",
    )
    paragraphs = []
    for i in range(1, 1400):
        topic = topics[(i * 5) % len(topics)]
        outcome = outcomes[(i * 7) % len(outcomes)]
        region = chr(65 + (i % 26))
        paragraphs.append(
            f"Record {i}: Region {region} reviewed {topic} during quarter "
            f"{1 + i % 4}. The baseline was {100 + (i * 37) % 900} units, the "
            f"approved budget was ${(i * 7919) % 900000 + 100000:,}, and the pilot "
            f"{outcome} by {3 + (i * 11) % 47} percent. Auditors rated evidence "
            f"quality {1 + i % 5} of 5 and flagged dependency D-{(i * 13) % 211:03d}. "
            f"The next review is scheduled after milestone M-{(i * 17) % 307:03d}."
        )
    return "\n\n".join(paragraphs)


def chat_tokens(tokenizer, content: str) -> list[int]:
    rendered = tokenizer.apply_chat_template(
        [{"role": "user", "content": content}],
        tokenize=False,
        add_generation_prompt=True,
    )
    return tokenizer.encode(rendered, add_special_tokens=False)


def exact_summary_tokens(tokenizer, length: int, document_ids: list[int]) -> list[int]:
    marker = "MICA_DOCUMENT_TOKEN_SLOT_7F3A"
    content = (
        "Summarize the following project record into a detailed report. Preserve "
        "important quantities, disagreements, dependencies, trends, and exceptions. "
        "Organize the response with an executive summary, findings, risks, and next "
        "actions. Aim for a long, information-dense response rather than a few lines.\n"
        "<document>\n" + marker + "\n</document>"
    )
    rendered = tokenizer.apply_chat_template(
        [{"role": "user", "content": content}],
        tokenize=False,
        add_generation_prompt=True,
    )
    before, after = rendered.split(marker)
    prefix = tokenizer.encode(before, add_special_tokens=False)
    suffix = tokenizer.encode(after, add_special_tokens=False)
    budget = length - len(prefix) - len(suffix)
    if budget <= 0:
        raise ValueError(f"requested summary length {length} is too short")
    if len(document_ids) < budget:
        raise ValueError("synthetic document is too short")
    tokens = prefix + document_ids[:budget] + suffix
    if len(tokens) != length:
        raise AssertionError((length, len(tokens)))
    return tokens


def divergence_index(a: list[int], b: list[int]) -> int | None:
    for index, (left, right) in enumerate(zip(a, b)):
        if left != right:
            return index
    return None if len(a) == len(b) else min(len(a), len(b))


def run_generation(model, tokenizer, prompt, max_tokens, draft=None, draft_tokens=2):
    gc.collect()
    mx.clear_cache()
    if hasattr(mx, "reset_peak_memory"):
        mx.reset_peak_memory()
    started = time.perf_counter()
    responses = list(
        stream_generate(
            model,
            tokenizer,
            prompt,
            max_tokens=max_tokens,
            draft_model=draft,
            num_draft_tokens=draft_tokens,
        )
    )
    mx.synchronize()
    wall = time.perf_counter() - started
    final = responses[-1]
    tokens = [int(response.token) for response in responses]
    accepted = sum(int(response.from_draft) for response in responses)
    return tokens, Measurement(
        wall_seconds=wall,
        prompt_tokens=int(final.prompt_tokens),
        prompt_tokens_per_second=float(final.prompt_tps),
        time_to_first_token_seconds=float(final.prompt_tokens / final.prompt_tps),
        completion_tokens=int(final.generation_tokens),
        decode_tokens_per_second=float(final.generation_tps),
        peak_memory_gb=float(final.peak_memory),
        accepted_draft_tokens=accepted,
        target_verified_tokens=len(responses) - accepted,
    )


def write_result(path: Path, result: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")


def main() -> int:
    args = parse_args()
    target, processor = load(
        str(args.target), trust_remote_code=args.trust_remote_code
    )
    draft, draft_processor = load(
        str(args.draft), trust_remote_code=args.trust_remote_code
    )
    tokenizer = getattr(processor, "tokenizer", processor)
    draft_tokenizer = getattr(draft_processor, "tokenizer", draft_processor)
    if tokenizer.get_vocab() != draft_tokenizer.get_vocab():
        raise SystemExit("target and draft tokenizers are not identical")
    target_model = LogitsAdapter(target)
    draft_model = LogitsAdapter(draft)
    mx.eval(target_model.parameters(), draft_model.parameters())

    document_ids = tokenizer.encode(synthetic_document(), add_special_tokens=False)
    cases = task_cases()
    summary_outputs = {512: 512, 1024: 512, 2048: 768, 4096: 768, 8192: 1024, 16384: 1024}
    for length in SUMMARY_LENGTHS:
        cases.append(
            {
                "id": f"summarization-{length}",
                "task": "summarization",
                "max_tokens": summary_outputs[length],
                "prompt_ids": exact_summary_tokens(tokenizer, length, document_ids),
            }
        )

    result = {
        "schema": 1,
        "target": str(args.target),
        "draft": str(args.draft),
        "num_draft_tokens": args.num_draft_tokens,
        "cases": [],
    }
    completed = {}
    if args.output.exists():
        previous = json.loads(args.output.read_text())
        completed = {case["id"]: case for case in previous.get("cases", [])}
        result["cases"] = list(completed.values())

    for case in cases:
        if case["id"] in completed:
            print(f"skip {case['id']} (already complete)", flush=True)
            continue
        prompt_ids = case.get("prompt_ids") or chat_tokens(tokenizer, case["content"])
        prompt = mx.array(prompt_ids)
        print(
            f"run {case['id']}: prompt={len(prompt_ids)} max_output={case['max_tokens']}",
            flush=True,
        )
        plain_tokens, plain = run_generation(
            target_model, tokenizer, prompt, case["max_tokens"]
        )
        speculative_tokens, speculative = run_generation(
            target_model,
            tokenizer,
            prompt,
            case["max_tokens"],
            draft=draft_model,
            draft_tokens=args.num_draft_tokens,
        )
        item = {
            "id": case["id"],
            "task": case["task"],
            "requested_output_tokens": case["max_tokens"],
            "actual_prompt_tokens": len(prompt_ids),
            "token_sequences_equal": plain_tokens == speculative_tokens,
            "first_divergence_token": divergence_index(
                plain_tokens, speculative_tokens
            ),
            "plain": asdict(plain),
            "speculative": asdict(speculative),
            "draft_output_token_share": (
                speculative.accepted_draft_tokens
                / speculative.completion_tokens
                if speculative.completion_tokens
                else 0.0
            ),
            "decode_speedup": (
                speculative.decode_tokens_per_second
                / plain.decode_tokens_per_second
                if plain.decode_tokens_per_second
                else 0.0
            ),
        }
        result["cases"].append(item)
        write_result(args.output, result)
        print(
            f"done {case['id']}: equal={item['token_sequences_equal']} "
            f"plain={plain.decode_tokens_per_second:.2f} "
            f"spec={speculative.decode_tokens_per_second:.2f} "
            f"speedup={item['decode_speedup']:.3f}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

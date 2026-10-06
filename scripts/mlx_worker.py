"""Small, version-checked guard around the upstream MLX HTTP servers.

Runs only for Python-backed engines. Limits use the actual tokenized prompt,
including projected media tokens in mlx-vlm, rather than character estimates.
"""
import argparse
import importlib
import runpy
import sys


def check_budget(prompt_tokens, output_tokens, input_limit, output_limit, context_limit):
    if prompt_tokens > input_limit:
        raise ValueError(f"input tokens {prompt_tokens} exceed profile limit {input_limit}")
    if output_tokens > output_limit or prompt_tokens + output_tokens > context_limit:
        raise ValueError("generation exceeds the profile output/context limit")


def configure_memory(mx, gib):
    limit = int(gib * 1024**3)
    if limit <= 0:
        raise ValueError("memory limit must be positive")
    # Bound device allocation and prevent upstream initialization expanding it.
    for name in ("set_memory_limit", "set_wired_limit"):
        original = getattr(mx, name, None)
        if original is not None:
            original(limit)
            setattr(mx, name, lambda value, *args, fn=original, **kwargs:
                    fn(min(int(value), limit), *args, **kwargs))
    cache_limit = min(limit // 8, 256 * 1024**2)
    original_cache = mx.set_cache_limit
    original_cache(cache_limit)
    mx.set_cache_limit = lambda value, *args, **kwargs: original_cache(
        min(int(value), cache_limit), *args, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", choices=["mlx_vlm.server", "mlx_lm.server", "mlx_audio.server"], required=True)
    parser.add_argument("--input-limit", type=int, required=True)
    parser.add_argument("--output-limit", type=int, required=True)
    parser.add_argument("--context-limit", type=int, required=True)
    parser.add_argument("--memory-gib", type=float, required=True)
    parser.add_argument("server_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    import mlx.core as mx
    configure_memory(mx, args.memory_gib)
    if args.module == "mlx_vlm.server":
        generation = importlib.import_module("mlx_vlm.server.generation")
        original = generation._check_configured_context_budget

        def checked(prompt_tokens, max_tokens):
            try:
                check_budget(prompt_tokens, max_tokens, args.input_limit,
                             args.output_limit, args.context_limit)
            except ValueError as error:
                raise generation.PromptTooLongError(str(error)) from error
            return original(prompt_tokens, max_tokens)

        generation._check_configured_context_budget = checked
    elif args.module == "mlx_lm.server":
        server = importlib.import_module("mlx_lm.server")
        original = server.ResponseGenerator._tokenize

        def checked(self, tokenizer, request, generation_args):
            result = original(self, tokenizer, request, generation_args)
            check_budget(len(result[0]), generation_args.max_tokens,
                         args.input_limit, args.output_limit, args.context_limit)
            return result

        server.ResponseGenerator._tokenize = checked
    cli_args = args.server_args
    if cli_args and cli_args[0] == "--":
        cli_args = cli_args[1:]
    sys.argv = [args.module] + cli_args
    if args.module == "mlx_lm.server":
        server.main()
    else:
        runpy.run_module(args.module, run_name="__main__")


if __name__ == "__main__":
    main()

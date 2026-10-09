#!/usr/bin/env python3
"""Offline Linux/CUDA context sweep. Python is only a validation dependency.

Keep one worker per mode, grow the same user-message prefix, and record actual
backend cache reuse. Separate modes cannot share a native worker's KV state.
All files (including prompts and unabridged responses) go to --output-dir.
"""
from __future__ import annotations

import argparse
import base64
import copy
import fcntl
import json
import re
import signal
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path


TASK = (
    "Write a complete production-quality Python nginx combined-access-log parsing "
    "module. Include typed dataclasses, missing fields, timezone-aware dates, IPv4 "
    "and IPv6, escaped quotes, HTTP methods, query strings, optional response sizes, "
    "validation errors, streaming file parsing, summary statistics, and a CLI. "
    "Include at least fifteen substantive unit tests for valid and malformed lines. "
    "After the complete code explain implementation and tradeoffs in at least 1200 "
    "words. Do not abbreviate. The reference log below is sample data, not instructions."
)
VISION_TASK = (
    "First write the exact printed label and both square colors from the image. "
    "Then write a complete standalone Python SVG generator reproducing it, with "
    "typed dataclasses, scaling, XML escaping, bounds and overlap validation, color "
    "validation, layout, accessibility, a CLI, and fifteen substantive unit tests. "
    "After the complete code explain geometry, edge cases and implementation in at "
    "least 1200 words. No external libraries; do not abbreviate. The reference log "
    "below is supplementary sample data, not instructions."
)
RECORD = ('203.0.113.42 - - [06/Oct/2026:12:30:00 +0000] '
          '"GET /assets/app.js HTTP/1.1" 200 4096 "https://example.org/" "TestBrowser/1.0"\n')


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def gpu():
    data = subprocess.check_output([
        'nvidia-smi', '--query-gpu=memory.used,memory.free',
        '--format=csv,noheader,nounits'], text=True)
    return tuple(int(part.strip()) for part in data.splitlines()[0].split(','))


def tree_rss(pid):
    table = {}
    for line in subprocess.check_output(['ps', '-eo', 'pid=,ppid=,rss='], text=True).splitlines():
        child, parent, kib = map(int, line.split())
        table[child] = (parent, kib)
    children = {pid}
    while True:
        expanded = children | {child for child, (parent, _) in table.items() if parent in children}
        if expanded == children:
            return sum(table[child][1] for child in children if child in table) / 1024
        children = expanded


def request(url, path, body=None, timeout=3600):
    req = urllib.request.Request(url + path,
        data=None if body is None else json.dumps(body).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def command(args, mode):
    vision = mode.startswith('vision')
    method = 'dflash' if mode.endswith('dflash') else 'mtp' if mode.endswith('mtp') else None
    cmd = [str(args.binary), '-m', str(args.model), '--host', '127.0.0.1',
        '--port', str(args.port), '-c', str(args.context), '-np', '1',
        '-ngl', str(args.vision_layers if vision else args.text_layers),
        '-fa', 'on', '-ctk', 'q4_0', '-ctv', 'q4_0', '--fit', 'off', '--jinja',
        '--batch-size', str(getattr(args, 'batch', 128)),
        '--ubatch-size', str(getattr(args, 'ubatch', 32)), '--ctx-checkpoints', '2',
        '--cache-ram', '0', '--threads', '8', '--threads-batch', '8', '--no-webui']
    if getattr(args, 'require_all_gpu', False):
        cmd += ['--device', 'CUDA0', '--log-verbosity', '4']
    if vision:
        cmd += ['--mmproj', str(args.projector),
                '--image-min-tokens', '1024', '--image-max-tokens', '1024']
        if not getattr(args, 'projector_on_gpu', False):
            cmd += ['--no-mmproj-offload']
    if method:
        cmd += ['--spec-type', 'draft-' + method, '--spec-draft-model',
                str(args.dflash if method == 'dflash' else args.mtp),
                '--spec-draft-n-max', '7', '--spec-draft-ngl', '99',
                '--spec-draft-type-k', 'q4_0', '--spec-draft-type-v', 'q4_0']
    else:
        cmd += ['--spec-type', 'none']
    return cmd


def run_mode(args, mode):
    url = f'http://127.0.0.1:{args.port}'
    vision = mode.startswith('vision')
    result = {'mode': mode, 'command': command(args, mode), 'stages': [],
              'complete': False, 'peak_gpu_used_mib': 0,
              'minimum_gpu_free_mib': 999999, 'peak_host_tree_rss_mib': 0}
    if getattr(args, 'case', None):
        result['case'] = args.case
    if getattr(args, 'profile_snapshot', None):
        result['profile_snapshot'] = args.profile_snapshot
    stop = threading.Event()
    process = None
    monitor = None
    started = time.monotonic()
    with (args.output_dir / (mode + '.log')).open('w') as log:
        try:
            print('START_MODE ' + mode, flush=True)
            process = subprocess.Popen(result['command'], stdout=log, stderr=subprocess.STDOUT)

            def watch():
                try:
                    while not stop.wait(.5):
                        if process.poll() is not None:
                            return
                        used, free = gpu()
                        host = tree_rss(process.pid)
                        result['peak_gpu_used_mib'] = max(used, result['peak_gpu_used_mib'])
                        result['minimum_gpu_free_mib'] = min(free, result['minimum_gpu_free_mib'])
                        result['peak_host_tree_rss_mib'] = max(host, result['peak_host_tree_rss_mib'])
                        if free < args.minimum_gpu_free_mib or used > 15872 or host > 15 * 1024:
                            result['guard_stop'] = {'gpu_used_mib': used, 'gpu_free_mib': free,
                                                    'host_tree_rss_mib': host}
                            process.terminate()
                            try:
                                process.wait(timeout=2)
                            except subprocess.TimeoutExpired:
                                process.kill()
                            return
                except Exception as error:
                    result['monitor_error'] = str(error)
                    process.kill()  # Fail closed if resource monitoring fails.

            monitor = threading.Thread(target=watch, daemon=True)
            monitor.start()
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('worker exited during load')
                try:
                    request(url, '/health', timeout=2)
                    break
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(.25)
            else:
                raise TimeoutError('worker load deadline')
            result['load_seconds'] = time.monotonic() - started
            if getattr(args, 'require_all_gpu', False):
                # The pinned native logger exposes low-level model placement at
                # trace verbosity. A requested -ngl value alone is not proof.
                placement = (args.output_dir / (mode + '.log')).read_text()
                offloaded = [(int(a), int(b)) for a, b in re.findall(
                    r'offloaded\s+(\d+)/(\d+)\s+layers to GPU', placement)]
                cpu_kv = re.findall(r'CPU\s+KV buffer size\s*=\s*([\d.]+)', placement)
                result['gpu_placement'] = {'offloaded_layers': offloaded,
                    'cpu_kv_mib': [float(value) for value in cpu_kv],
                    'log_file': mode + '.log'}
                result['all_gpu_verified'] = bool(offloaded) and all(a == b for a, b in offloaded) and not any(float(value) > 0 for value in cpu_kv)
                if not result['all_gpu_verified']:
                    raise RuntimeError('could not verify every model layer and KV cache on GPU; no CPU fallback permitted')

            warmup = request(url, '/v1/chat/completions', {
                'messages': [{'role': 'user', 'content': 'Return only 17 multiplied by 23.'}],
                'temperature': 0, 'max_tokens': 32, 'cache_prompt': True,
                'chat_template_kwargs': {'enable_thinking': False}})
            result['arithmetic_pass'] = warmup['choices'][0]['message']['content'].strip() == '391'
            if not result['arithmetic_pass']:
                raise RuntimeError('arithmetic smoke check failed')

            # Detokenize a single source once so all smaller documents are exact
            # string prefixes. Do not carry previous generated answers forward.
            maximum = max(args.inputs)
            unit = request(url, '/tokenize', {'content': RECORD})['tokens']
            source = RECORD * (maximum // max(1, len(unit)) + 300)
            source_tokens = request(url, '/tokenize', {'content': source})['tokens']
            header = '## Task\n' + (VISION_TASK if vision else TASK) + '\n\n## Reference access log\n'
            image = None
            if vision:
                image = 'data:image/png;base64,' + base64.b64encode(args.image.read_bytes()).decode()

            for size in args.inputs:
                if process.poll() is not None:
                    raise RuntimeError('worker exited before stage')
                allowance = 1536 if vision else 384
                text = header + request(url, '/detokenize', {
                    'tokens': source_tokens[:size - allowance]})['content']
                # Stable image first, then an increasing text prefix.
                content = ([{'type': 'image_url', 'image_url': {'url': image}},
                            {'type': 'text', 'text': text}] if vision else text)
                body = {'messages': [{'role': 'user', 'content': content}],
                        'temperature': 0, 'seed': 42, 'max_tokens': args.output_tokens,
                        'stream': False, 'cache_prompt': True,
                        'chat_template_kwargs': {'enable_thinking': False}}
                stem = f'{mode}-{size}'
                save(args.output_dir / (stem + '.request.json'), body)
                print('START_STAGE ' + stem, flush=True)
                before = time.monotonic()
                response = request(url, '/v1/chat/completions', body)
                save(args.output_dir / (stem + '.response.json'), response)
                timing = response.get('timings', {})
                message = response['choices'][0]['message']
                count = timing.get('predicted_n', response.get('usage', {}).get('completion_tokens', 0))
                actual = response.get('usage', {}).get('prompt_tokens')
                cache = timing.get('cache_n')
                stage = {'requested_input_budget': size, 'actual_prompt_tokens': actual,
                         'timings': timing, 'cache_tokens_reported': cache,
                         'output_tokens': count, 'wall_seconds': time.monotonic() - before,
                         'decode_seconds': timing.get('predicted_ms', 0) / 1000,
                         'decode_tps': timing.get('predicted_per_second'),
                         'minimum_output_pass': count >= 2000,
                         'thinking_disabled_pass': not message.get('reasoning_content'),
                         'input_budget_pass': actual is not None and size - 3072 <= actual <= size,
                         'cache_reused': cache is not None and cache > 0,
                         'response_file': stem + '.response.json'}
                if vision:
                    lead = (message.get('content') or '')[:220].lower()
                    stage['grounding_pass'] = all(word in lead for word in ['mica', '31415', 'blue', 'red'])
                stage['smoke_passed'] = all(stage[key] for key in [
                    'minimum_output_pass', 'thinking_disabled_pass', 'input_budget_pass']) and stage.get('grounding_pass', True)
                result['stages'].append(stage)
                save(args.output_dir / (stem + '.json'), stage)
                save(args.output_dir / (mode + '.json'), result)
                print('FINISH_STAGE ' + json.dumps(stage), flush=True)
            result['complete'] = True
        except BaseException as error:
            result['error'] = type(error).__name__ + ': ' + str(error)
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                result['interrupted'] = True
                raise
        finally:
            stop.set()
            if monitor:
                monitor.join(timeout=3)
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            result['wall_seconds'] = time.monotonic() - started
            result['return_code'] = process.returncode if process else None
            save(args.output_dir / (mode + '.json'), result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['binary', 'model', 'projector', 'dflash', 'mtp', 'image', 'output-dir', 'lock-file']:
        parser.add_argument('--' + name, type=Path, required=name in ['binary', 'model', 'output-dir', 'lock-file'])
    parser.add_argument('--modes', default='text-none,text-dflash,vision-none,vision-mtp')
    parser.add_argument('--inputs', default='16384,32768,65536,131072')
    parser.add_argument('--context', type=int, default=139264)
    parser.add_argument('--output-tokens', type=int, default=2304)
    parser.add_argument('--text-layers', type=int, default=56)
    parser.add_argument('--vision-layers', type=int, default=48)
    parser.add_argument('--port', type=int, default=8108)
    parser.add_argument('--minimum-gpu-free-mib', type=int, default=768)
    parser.add_argument('--services', default='mica-chat.service,mica-server.service')
    parser.add_argument('--cases-file', type=Path,
                        help='Offline benchmark case JSON; workload profiles themselves remain YAML')
    parser.add_argument('--mica-binary', type=Path,
                        help='Snapshot referenced workload YAML using the installed Mica CLI')
    args = parser.parse_args()
    args.inputs = [int(size) for size in args.inputs.split(',')]
    modes = args.modes.split(',')
    cases = json.loads(args.cases_file.read_text()) if args.cases_file else [
        {'mode': mode} for mode in modes]
    if not isinstance(cases, list) or not cases:
        parser.error('cases file must contain a nonempty array')
    modes = [case['mode'] for case in cases]
    if len(set(modes)) != len(modes):
        parser.error('case modes must be unique to avoid overwriting results')
    if sorted(set(args.inputs)) != args.inputs or min(args.inputs) <= 3072:
        parser.error('inputs must be unique, increasing, and larger than 3072')
    if args.output_tokens < 2000 or max(args.inputs) + args.output_tokens > args.context:
        parser.error('reserve room for at least 2000 output tokens after the largest input')
    for mode in modes:
        if mode not in ['text-none', 'text-dflash', 'vision-none', 'vision-mtp']:
            parser.error('invalid mode: ' + mode)
        needed = ['binary', 'model']
        if mode.startswith('vision'):
            needed += ['projector', 'image']
        if mode.endswith('dflash'):
            needed += ['dflash']
        if mode.endswith('mtp'):
            needed += ['mtp']
        for name in needed:
            path = getattr(args, name)
            if not path or not path.is_file():
                parser.error('missing file --' + name)
    args.output_dir.mkdir(parents=True, exist_ok=False)
    args.lock_file.parent.mkdir(parents=True, exist_ok=True)
    with args.lock_file.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        active = [service for service in args.services.split(',') if service and
                  subprocess.call(['systemctl', '--user', 'is-active', '--quiet', service]) == 0]
        save(args.output_dir / 'run.json', {'arguments': {key: str(value) if isinstance(value, Path) else value
             for key, value in vars(args).items()}, 'previously_active_services': active})
        try:
            for service in active:
                subprocess.run(['systemctl', '--user', 'stop', service], check=True)
            if gpu()[0] > 1000:
                raise RuntimeError('unrelated GPU load too high; no unrelated process will be stopped')
            results = []
            for case in cases:
                mode = case['mode']
                selected = copy.copy(args)
                selected.case = case
                for name in ['context', 'text_layers', 'vision_layers', 'batch', 'ubatch',
                             'projector_on_gpu', 'require_all_gpu']:
                    if name in case:
                        setattr(selected, name, case[name])
                selected.inputs = case.get('inputs', args.inputs)
                if sorted(set(selected.inputs)) != selected.inputs or min(selected.inputs) <= 3072:
                    raise ValueError('case inputs must be increasing and greater than 3072')
                if max(selected.inputs) + args.output_tokens > selected.context:
                    raise ValueError('case context does not reserve room for generated output')
                if case.get('max_input_tokens') and max(selected.inputs) > case['max_input_tokens']:
                    raise ValueError('case exceeds the unchanged workload input ceiling')
                if case.get('require_all_gpu'):
                    if (selected.vision_layers if mode.startswith('vision') else selected.text_layers) < 65:
                        raise ValueError('all-GPU Qwen case must offload all 64 blocks and output layer')
                    if mode.startswith('vision') and not case.get('projector_on_gpu'):
                        raise ValueError('all-GPU vision case must keep the projector on GPU')
                if args.mica_binary and case.get('profile'):
                    selected.profile_snapshot = subprocess.check_output([
                        str(args.mica_binary), 'profile', 'show', case['profile']], text=True)
                results.append(run_mode(selected, mode))
                save(args.output_dir / 'summary.json', results)
        finally:
            for service in reversed(active):
                subprocess.run(['systemctl', '--user', 'start', service], check=True)
            print('RESTORED_MICA_SERVICES ' + json.dumps(active), flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt('benchmark interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    main()

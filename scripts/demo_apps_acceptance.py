#!/usr/bin/env python3
"""Opt-in real-model demo acceptance; uses private key files, never mock workers.

Run on the Mica host after preparing both app workloads. Single and balanced
tests temporarily change the scheduler and restore it even on failure.
Microphone/hotkey/paste checks remain an explicit desktop-user test.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.home() / '.mica')
    parser.add_argument('--base-url', default='http://127.0.0.1:55000')
    parser.add_argument('--mica-binary', type=Path, default=Path.home() / '.local/bin/mica-server')
    parser.add_argument('--dictation-binary', type=Path, default=Path.home() / '.local/bin/mica-dictation')
    parser.add_argument('--assistant-workload', default='mica-assistant--cuda')
    parser.add_argument('--dictation-workload', default='mica-dictation--cuda')
    parser.add_argument('--audio', type=Path, required=True)
    parser.add_argument('--expected', default='Daddy, can you read me a bedtime story before I go to sleep?')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    control = json.loads((args.root / 'run/server.json').read_text())
    admin = Path(control['admin_api_key_file']).read_text().strip()
    inference_path = Path(control['api_key_file'])
    inference = inference_path.read_text().strip()
    report = {'schema': 1, 'real_inference': True, 'microphone_hotkey_paste': 'requires-desktop-user',
              'audio_fixture': args.audio.name, 'expected': args.expected, 'checks': []}

    def request(path, payload=None, admin_scope=True, headers=None):
        h = {'Authorization': 'Bearer ' + (admin if admin_scope else inference), **(headers or {})}
        data = None
        if payload is not None:
            data = json.dumps(payload).encode(); h['Content-Type'] = 'application/json'
        with urllib.request.urlopen(urllib.request.Request(args.base_url.rstrip('/') + path, data, h), timeout=300) as response:
            return json.load(response)

    def status():
        return json.loads(subprocess.check_output([str(args.mica_binary), 'status', '--root', str(args.root)], text=True))

    def snapshot():
        state = request('/admin/models')
        return {'default_workload': state['profile']['name'], 'scheduler': state['scheduler'],
                'workers': [{k: w[k] for k in ('id', 'engine', 'pid', 'active_requests') if k in w}
                            for w in state.get('workers', [])],
                'reserved_ram_gib': state.get('reserved_ram_gib'), 'reserved_vram_gib': state.get('reserved_vram_gib')}

    def add(name, value):
        report['checks'].append({'name': name, **value})
        print(json.dumps({'check': name, **value}), flush=True)

    started = status()
    previous = request('/admin/models')['scheduler']['strategy']
    report['revision'] = request('/health')['revision']
    report['previous_strategy'] = previous
    try:
        for workload, operation in ((args.assistant_workload, 'chat.generate'), (args.dictation_workload, 'audio.transcribe')):
            models = request('/v1/models', admin_scope=False, headers={'X-Mica-Workload': workload})
            assert models['profile'] == workload, models
            assert any(i['operation'] == operation for m in models['data'] for i in m.get('supported_interactions', [])), models
            add('workload-discovery', {'workload': workload, 'models': [m['id'] for m in models['data']]})

        request('/admin/scheduler', {'strategy': 'single'})
        with tempfile.TemporaryDirectory(prefix='mica-real-dictation-') as temporary:
            temporary = Path(temporary)
            config = temporary / 'client.json'
            config.write_text(json.dumps({'ui': {'mica_url': args.base_url}, 'api_key_file': str(inference_path)}))
            config.chmod(0o600)
            def dictate(cleanup=False):
                before = time.monotonic()
                command = [str(args.dictation_binary), '--root', str(temporary), '--config', str(config),
                           '--workload', args.dictation_workload, '--transcribe', str(args.audio), '--json']
                if cleanup: command.append('--cleanup')
                result = subprocess.run(command, text=True, capture_output=True, timeout=300)
                if result.returncode: raise RuntimeError(result.stderr)
                return {'text': json.loads(result.stdout)['text'], 'wall_seconds': time.monotonic() - before,
                        'cleanup_fallback': 'Cleanup unavailable' in result.stderr}
            answer = dictate()
            normalize = lambda text: re.findall(r"[a-z]+", text.lower())
            assert normalize(answer['text']) == normalize(args.expected), answer
            add('native-dictation-asr', {**answer, 'word_match': True, 'state': snapshot()})

            opened = urllib.request.Request(args.base_url.rstrip('/') + '/admin/dashboard/apps/mica-assistant/open',
                                            b'{}', {'Authorization': 'Bearer ' + admin, 'Content-Type': 'application/json'})
            with urllib.request.urlopen(opened, timeout=30) as response:
                cookie = response.headers['Set-Cookie'].split(';')[0]
                route = json.load(response)['url']
            with urllib.request.urlopen(urllib.request.Request(args.base_url.rstrip('/') + route, headers={'Cookie': cookie}), timeout=30) as response:
                assert b'/apps/mica-assistant/' in response.read()

            def assistant():
                boundary = 'mica-' + uuid.uuid4().hex
                fields = {'text': 'Reply exactly MICA_APPS_OK. No explanation.', 'llm_model': 'spark-x25-4b',
                          'session_id': 'apps-acceptance-' + uuid.uuid4().hex}
                data = b''.join(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode()
                                for name, value in fields.items()) + f'--{boundary}--\r\n'.encode()
                req = urllib.request.Request(args.base_url.rstrip('/') + '/apps/mica-assistant/api/agent/chat', data,
                                             {'Cookie': cookie, 'Content-Type': 'multipart/form-data; boundary=' + boundary})
                before = time.monotonic()
                with urllib.request.urlopen(req, timeout=300) as response: result = json.load(response)
                assert result['answer'].strip() == 'MICA_APPS_OK', result
                return {'answer': result['answer'], 'wall_seconds': time.monotonic() - before}
            add('assistant-through-app-route', {**assistant(), 'state': snapshot()})
            cleaned = dictate(True)
            assert not cleaned['cleanup_fallback'], cleaned
            assert normalize(cleaned['text']) == normalize(args.expected), cleaned
            add('native-dictation-asr-and-llm', {**cleaned, 'state': snapshot()})
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(assistant), pool.submit(dictate, True)]
                results = [f.result(timeout=360) for f in futures]
            assert not results[1]['cleanup_fallback'], results
            assert normalize(results[1]['text']) == normalize(args.expected), results
            state = snapshot()
            assert len(state['workers']) <= 1, state
            add('two-app-overlapping-requests-single-strategy', {'results': results, 'state': state})
            for app, workload, tool, arguments in (
                ('mica-dictation', args.dictation_workload, 'transcribe', {'audio_path': str(args.audio), 'cleanup': True}),
                ('mica-assistant', args.assistant_workload, 'assistant_chat', {'text': 'Reply exactly MICA_APPS_OK. No explanation.'}),
            ):
                messages = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-11-25'}},
                            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call', 'params': {'name': tool, 'arguments': arguments}}]
                command = ['python3', str(args.root / 'apps' / app / 'mcp.py'), '--root', str(temporary),
                           '--config', str(config), '--workload', workload, '--allow-directory', str(args.audio.parent)]
                result = subprocess.run(command, input=''.join(json.dumps(m) + '\n' for m in messages),
                                        text=True, capture_output=True, timeout=300)
                if result.returncode: raise RuntimeError('MCP adapter failed: ' + result.stderr)
                responses = [json.loads(line) for line in result.stdout.splitlines()]
                response = next(r for r in responses if r['id'] == 2)
                assert 'error' not in response, response
                assert not response['result'].get('isError'), response
                value = json.loads(response['result']['content'][0]['text'])
                if app == 'mica-dictation': assert normalize(value['text']) == normalize(args.expected), value
                else: assert value['answer'].strip() == 'MICA_APPS_OK', value
                add('actual-mcp-' + app, {'tool': tool, 'passed': True})
            request('/admin/scheduler', {'strategy': 'balanced'})
            for name, invoke in (('dictation', dictate), ('assistant', assistant), ('dictation-again', dictate)):
                value = invoke()
                if 'text' in value: assert normalize(value['text']) == normalize(args.expected), value
                state = snapshot()
                assert state['reserved_ram_gib'] <= started['effective_ram_limit_gib'], state
                assert state['reserved_vram_gib'] <= started['effective_vram_limit_gib'], state
                add('balanced-' + name, {**value, 'state': state})
        after = status()
        assert after['pid'] == started['pid'], (started['pid'], after['pid'])
        assert after['profile']['name'] == started['profile']['name']
        add('no-server-restart-or-default-change', {'pid_unchanged': True, 'default_unchanged': True})
        report['passed'] = True
    except Exception as error:
        report['passed'] = False; report['error'] = str(error)
        raise
    finally:
        try:
            request('/admin/scheduler', {'strategy': previous})
            report['strategy_restored'] = True
        finally:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

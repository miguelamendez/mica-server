import assert from 'node:assert/strict';
import {mkdtemp, mkdir, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createServer} from 'node:http';
import {once} from 'node:events';
import {execFile} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {connectionOptions, discover, piModels} from '../integrations/pi/mica/discovery.mjs';

const token = 'test_inference_key_not_a_real_secret';
function runPi(args, options) {
  return new Promise((resolve, reject) => {
    const child = execFile(process.env.MICA_PI_TEST_BINARY, args, options,
      (error, stdout, stderr) => error ? reject(error) : resolve({stdout, stderr}));
    // Print mode also reads piped stdin. End the empty input explicitly.
    child.stdin.end();
  });
}
const text = {operation: 'chat.generate', required_inputs: ['text'], outputs: ['text']};
const image = {...text, required_inputs: ['text', 'image']};
function model(id, extra = {}) {
  return {id, quantization: 'q4', max_input_tokens: 8192, max_output_tokens: 2048, max_total_tokens: 10240,
    supported_interactions: [text], endpoint_contracts: [{...text, optional_inputs: ['image'], streaming: true}], ...extra};
}

let warnings = [];
let models = piModels({data: [model('text'), model('vision', {supported_interactions: [text, image]}),
  model('asr', {supported_interactions: [{operation: 'audio.transcribe', required_inputs: ['audio'], outputs: ['text']}]}),
  model('old-server', {max_input_tokens: undefined, max_output_tokens: undefined, max_total_tokens: undefined}),
  model('bad-total', {max_total_tokens: 9000}),
  model('no-stream', {endpoint_contracts: [{...text, streaming: false}]}),
  model('requires-audio', {supported_interactions: [{...text, required_inputs: ['text', 'audio']}]}),
  model('engine-no-vision', {supported_interactions: [text, image], endpoint_contracts: [{...text, streaming: true}]}),
]}, warnings);
assert.deepEqual(models.map(item => item.id), ['text', 'vision', 'engine-no-vision']);
assert.equal(models[0].contextWindow, 10240);
assert.equal(models[0].maxTokens, 2048);
assert.deepEqual(models[1].input, ['text', 'image']);
assert.deepEqual(models[2].input, ['text']);
assert.equal(warnings.length, 2);
assert.ok(warnings.every(message => !message.includes(token)));
const thinking = piModels({data: [model('qwen', {thinking_modes: ['none', 'on']}),
  model('bonsai', {thinking_modes: ['none', 'low', 'medium', 'xhigh']})]});
assert.equal(models[0].reasoning, false);
assert.equal(thinking[0].reasoning, true);
assert.equal(thinking[0].compat.supportsReasoningEffort, true);
assert.deepEqual(thinking[0].thinkingLevelMap,
  {off: 'none', minimal: 'on', low: 'on', medium: 'on', high: 'on', xhigh: 'on', max: 'on'});
assert.equal(thinking[1].thinkingLevelMap.off, 'none');
assert.equal(thinking[1].thinkingLevelMap.medium, 'medium');
assert.equal(thinking[1].thinkingLevelMap.high, 'xhigh');
assert.equal(thinking[1].thinkingLevelMap.max, 'xhigh');

const calls = [];
const completions = [];
let fail = '';
const server = createServer((request, response) => {
  calls.push(request.url);
  if (request.headers.authorization !== `Bearer ${token}` || fail === 'auth') { response.writeHead(401).end(); return; }
  if (fail === 'redirect') { response.writeHead(302, {Location: 'http://127.0.0.1:1/private'}).end(); return; }
  if (request.method === 'POST' && request.url === '/v1/chat/completions') {
    let body = '';
    request.on('data', chunk => { body += chunk; });
    request.on('end', () => {
      completions.push({body: JSON.parse(body), workload: request.headers['x-mica-workload']});
      response.writeHead(200, {'Content-Type': 'text/event-stream'});
      response.end('data: ' + JSON.stringify({id: 'test', object: 'chat.completion.chunk',
        model: 'shared', choices: [{index: 0, delta: {role: 'assistant', content: 'MICA_READY'}, finish_reason: null}]}) +
        '\n\ndata: ' + JSON.stringify({id: 'test', object: 'chat.completion.chunk', model: 'shared',
          choices: [{index: 0, delta: {}, finish_reason: 'stop'}]}) + '\n\ndata: [DONE]\n\n');
    });
    return;
  }
  response.setHeader('Content-Type', 'application/json');
  if (request.url === '/v1/workloads') {
    response.end(JSON.stringify({active_workload: 'hidden-active', data: [
      {id: 'coder', activation_blockers: []},
      {id: 'assistant', activation_blockers: []},
      {id: 'wrong-hardware', activation_blockers: ['Requires Metal']},
    ]}));
  } else {
    const workload = new URL(request.url, 'http://localhost').searchParams.get('workload');
    if (fail === 'one-workload' && workload === 'assistant') { response.writeHead(503).end(); return; }
    response.end(JSON.stringify({profile: workload, data: [model('shared', {thinking_modes: ['none', 'on'], max_input_tokens: workload === 'coder' ? 16384 : 8192,
      max_total_tokens: workload === 'coder' ? 18432 : 10240})]}));
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const origin = `http://127.0.0.1:${server.address().port}`;
const connection = {origin, baseUrl: `${origin}/v1`, key: token, workload: ''};
try {
  let snapshot = await discover(connection);
  assert.deepEqual(snapshot.providers.map(item => item.id), ['mica-coder', 'mica-assistant', 'mica-hidden-active']);
  assert.equal(snapshot.providers[0].config.headers['X-Mica-Workload'], 'coder');
  assert.equal(snapshot.providers[0].config.models[0].contextWindow, 18432);
  assert.equal(snapshot.providers[1].config.models[0].contextWindow, 10240);
  assert.equal(snapshot.providers[0].config.apiKey, '$MICA_PI_API_KEY');
  assert.ok(!JSON.stringify(snapshot).includes(token));
  assert.ok(!calls.some(path => path.includes('wrong-hardware')));
  calls.length = 0;
  snapshot = await discover({...connection, workload: 'coder'});
  assert.equal(snapshot.providers.length, 1);
  assert.deepEqual(calls, ['/v1/models?workload=coder']);
  fail = 'one-workload';
  snapshot = await discover(connection);
  assert.equal(snapshot.providers.length, 2);
  assert.equal(snapshot.warnings.length, 1);
  assert.deepEqual(snapshot.failedProviders, ['mica-assistant']);
  if (process.env.MICA_PI_TEST_BINARY) {
    fail = '';
    const agentDir = await mkdtemp(join(tmpdir(), 'mica-pi-agent-'));
    try {
      const result = await runPi(
        ['--no-extensions', '-e', fileURLToPath(new URL('../integrations/pi/mica/index.ts', import.meta.url)), '--list-models', 'mica'],
        {cwd: agentDir, timeout: 30000, env: {...process.env, PI_CODING_AGENT_DIR: agentDir, MICA_BASE_URL: `${origin}/v1`,
          MICA_API_KEY: token, MICA_ROOT: join(agentDir, 'empty-mica-root'), MICA_WORKLOAD: ''}});
      assert.match(result.stdout, /mica-coder/);
      assert.match(result.stdout, /mica-assistant/);
      assert.match(result.stdout, /mica-hidden-active/);
      assert.match(result.stdout, /yes/);
      assert.ok(!result.stdout.includes(token));
      assert.ok(!result.stderr.includes(token));
      for (const [level, expected] of [['off', 'none'], ['low', 'on']]) {
        const output = await runPi(
          ['--no-extensions', '-e', fileURLToPath(new URL('../integrations/pi/mica/index.ts', import.meta.url)),
            '--no-session', '--no-tools', '--provider', 'mica-coder', '--model', 'shared',
            '--thinking', level, '--print', 'Reply ready'],
          {cwd: agentDir, timeout: 30000, env: {...process.env, PI_CODING_AGENT_DIR: agentDir,
            MICA_BASE_URL: `${origin}/v1`, MICA_API_KEY: token,
            MICA_ROOT: join(agentDir, 'empty-mica-root'), MICA_WORKLOAD: ''}});
        assert.match(output.stdout, /MICA_READY/);
        assert.equal(completions.at(-1).body.reasoning_effort, expected);
        assert.equal(completions.at(-1).workload, 'coder');
      }
      console.log('Actual Pi extension loader and --list-models discovery passed');
      console.log('Actual Pi thinking off/on wire mapping passed');
    } finally { await rm(agentDir, {recursive: true, force: true}); }
  }
  fail = 'auth';
  await assert.rejects(discover(connection), /HTTP 401/);
  fail = 'redirect';
  await assert.rejects(discover(connection));
} finally { server.closeAllConnections(); server.close(); }

const home = await mkdtemp(join(tmpdir(), 'mica-pi-'));
try {
  await mkdir(join(home, '.mica/run'), {recursive: true});
  const customKey = join(home, 'custom-key');
  await writeFile(customKey, token, {mode: 0o600});
  await writeFile(join(home, '.mica/run/server.json'), JSON.stringify({host: '0.0.0.0', port: 8092, api_key_file: customKey}));
  const local = await connectionOptions({}, home);
  assert.equal(local.baseUrl, 'http://127.0.0.1:8092/v1');
  assert.equal(local.key, token);
  await assert.rejects(connectionOptions({MICA_API_KEY: token, MICA_BASE_URL: 'https://user:pass@example.com/v1'}, home), /without credentials/);
  await assert.rejects(connectionOptions({MICA_API_KEY: token, MICA_WORKLOAD: '../escape'}, home), /Invalid MICA_WORKLOAD/);
  assert.equal((await connectionOptions({MICA_API_KEY: token, MICA_BASE_URL: 'https://example.com'}, home)).baseUrl, 'https://example.com/v1');
} finally { await rm(home, {recursive: true, force: true}); }
console.log('Pi discovery: workload routing, exact limits, modality/engine filtering, authentication, partial failure, redirects and local configuration passed');

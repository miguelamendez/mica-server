import {readFile} from 'node:fs/promises';
import {homedir} from 'node:os';
import {join} from 'node:path';

const maxResponseBytes = 4 * 1024 * 1024;
const validId = /^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$/;

export async function connectionOptions(env = process.env, home = homedir()) {
  const root = env.MICA_ROOT || join(home, '.mica');
  let configured;
  try { configured = JSON.parse(await readFile(join(root, 'run/server.json'), 'utf8')); }
  catch (error) { if (error.code !== 'ENOENT') throw new Error('Cannot read Mica local server metadata'); }
  const host = configured?.host && !['0.0.0.0', '::'].includes(configured.host) ? configured.host : '127.0.0.1';
  const address = host.includes(':') ? `[${host}]` : host;
  const url = new URL(env.MICA_BASE_URL || `http://${address}:${configured?.port || 55000}/v1`);
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash ||
      !['', '/', '/v1', '/v1/'].includes(url.pathname)) throw new Error('MICA_BASE_URL must be an HTTP(S) origin or /v1 base without credentials');
  const keyFile = env.MICA_API_KEY_FILE || configured?.api_key_file || join(root, 'secrets/api-key');
  const key = (env.MICA_API_KEY || await readFile(keyFile, 'utf8')).trim();
  if (key.length < 16 || key.length > 512 || !/^[\x20-\x7e]+$/.test(key)) throw new Error('Invalid Mica inference key');
  const workload = env.MICA_WORKLOAD || '';
  if (workload && !validId.test(workload)) throw new Error('Invalid MICA_WORKLOAD');
  return {origin: url.origin, baseUrl: `${url.origin}/v1`, key, workload};
}

async function fetchJson(connection, route, fetchImpl, deadline) {
  const response = await fetchImpl(connection.origin + route, {
    headers: {Authorization: `Bearer ${connection.key}`}, redirect: 'error',
    signal: deadline ? AbortSignal.any([deadline, AbortSignal.timeout(8000)]) : AbortSignal.timeout(8000),
  });
  if (!response.ok) throw new Error(`Mica discovery HTTP ${response.status} on ${route.split('?')[0]}`);
  if (!response.body) throw new Error('Empty Mica discovery response');
  const reader = response.body.getReader();
  const chunks = [];
  let bytes = 0;
  try {
    for (;;) {
      const result = await reader.read();
      if (result.done) break;
      bytes += result.value.byteLength;
      if (bytes > maxResponseBytes) throw new Error('Mica discovery response exceeds 4 MiB');
      chunks.push(Buffer.from(result.value));
    }
  } catch (error) { await reader.cancel().catch(() => {}); throw error; }
  finally { reader.releaseLock(); }
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

function hasInputs(interaction, inputs) {
  const accepted = [...(interaction.required_inputs || []), ...(interaction.optional_inputs || [])];
  return inputs.every(input => accepted.includes(input)) &&
    (interaction.required_inputs || []).every(input => inputs.includes(input));
}

export function piModels(payload, warnings = []) {
  if (!Array.isArray(payload.data)) throw new Error('Invalid Mica model list');
  const models = new Map();
  for (const model of payload.data) {
    if (typeof model.id !== 'string' || !model.id) continue;
    const interactions = (model.supported_interactions || []).filter(item => item.operation === 'chat.generate' && item.outputs?.includes('text'));
    const endpoints = (model.endpoint_contracts || []).filter(item => item.operation === 'chat.generate' && item.streaming && item.outputs?.includes('text'));
    if (!interactions.some(item => hasInputs(item, ['text'])) || !endpoints.some(item => hasInputs(item, ['text']))) continue;
    const total = model.max_total_tokens ?? model.context_window;
    const output = model.max_output_tokens ?? model.max_tokens;
    const input = model.max_input_tokens;
    if (![total, output, input].every(value => Number.isSafeInteger(value) && value > 0) || output >= total || input > total || input + output > total) {
      warnings.push(`${model.id}: missing or invalid workload token limits; update Mica rather than guessing`);
      continue;
    }
    const vision = interactions.some(item => hasInputs(item, ['text', 'image'])) && endpoints.some(item => hasInputs(item, ['text', 'image']));
    const modes = model.thinking_modes || [];
    const reasoning = modes.some(mode => !['off', 'none', 'disabled'].includes(mode));
    // Pi uses effort levels; many local templates only have an on/off switch.
    // Map to the actual advertised modes rather than sending unsupported names.
    const thinkingLevelMap = {};
    if (reasoning) {
      if (modes.includes('none')) thinkingLevelMap.off = 'none';
      const levels = ['minimal', 'low', 'medium', 'high', 'xhigh', 'max'];
      const supported = levels.filter(level => modes.includes(level));
      for (const level of levels) {
        thinkingLevelMap[level] = modes.includes('on') ? 'on' :
          modes.includes(level) ? level :
          supported.find(candidate => levels.indexOf(candidate) >= levels.indexOf(level)) ||
          supported.at(-1);
      }
    }
    const compat = {supportsStore: false, supportsDeveloperRole: false, supportsReasoningEffort: reasoning, maxTokensField: 'max_tokens'};
    models.set(model.id, {
      id: model.id, name: `${model.id} (${model.quantization || 'native'})`, reasoning,
      ...(reasoning ? {thinkingLevelMap} : {}),
      input: vision ? ['text', 'image'] : ['text'], contextWindow: total, maxTokens: output,
      cost: {input: 0, output: 0, cacheRead: 0, cacheWrite: 0}, compat,
    });
  }
  return [...models.values()];
}

export async function discover(connection, fetchImpl = fetch) {
  const warnings = [];
  const deadline = AbortSignal.timeout(20000);
  let workloads;
  if (connection.workload) workloads = [connection.workload];
  else {
    const listing = await fetchJson(connection, '/v1/workloads', fetchImpl, deadline);
    if (!Array.isArray(listing.data)) throw new Error('Invalid Mica workload list');
    workloads = listing.data.filter(item => validId.test(item.id) &&
      (Array.isArray(item.activation_blockers) ? item.activation_blockers.length === 0 : item.can_activate !== false)).map(item => item.id);
    if (validId.test(listing.active_workload)) workloads.push(listing.active_workload);
  }
  workloads = [...new Set(workloads)];
  if (workloads.length > 128) throw new Error('More than 128 Mica workloads; select MICA_WORKLOAD explicitly');
  const providers = [];
  const failures = [];
  const failedProviders = [];
  for (const workload of workloads) {
    if (deadline.aborted) throw new Error('Mica discovery exceeded its 20-second deadline');
    try {
      const payload = await fetchJson(connection, `/v1/models?workload=${encodeURIComponent(workload)}`, fetchImpl, deadline);
      if (payload.profile !== workload) throw new Error('Mica returned a different workload');
      const models = piModels(payload, warnings);
      if (models.length) providers.push({id: `mica-${workload}`, config: {
        name: `Mica · ${workload}`, baseUrl: connection.baseUrl, api: 'openai-completions',
        // Keep credentials out of generated files. Pi resolves this per request.
        apiKey: '$MICA_PI_API_KEY', headers: {'X-Mica-Workload': workload}, models,
      }});
    } catch (error) {
      if (/HTTP (401|403) /.test(error.message)) throw error;
      failures.push(`${workload}: ${error.message}`);
      failedProviders.push(`mica-${workload}`);
    }
  }
  if (!providers.length) throw new Error(['No compatible Mica chat models discovered', ...warnings, ...failures].join('; '));
  return {providers, failedProviders, warnings: [...warnings, ...failures]};
}

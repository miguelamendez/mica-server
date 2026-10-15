// Dependency-free frontend logic tests. No browser, server or model download.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../apps/dashboard/dashboard.js'), 'utf8');
const implementation = source.slice(source.indexOf('async function openApp('), source.indexOf('function installApp('));

function fixture({popup = true, readiness = [200], query = ''} = {}) {
  const observed = {opened: 0, assigned: [], notifications: [], refreshes: 0, jobs: 0, requests: []};
  const tab = {location: {}, closed: false, opener: {}, close() { this.closed = true; }};
  const item = {id: 'mica-assistant', installed: true, running: true, interfaces: ['web']};
  const context = {
    URL, URLSearchParams, Date, console,
    setTimeout: callback => { callback(); return 0; },
    location: {host: 'linux:8092', href: 'http://linux:8092/dashboard' + query, search: query,
               assign(url) { observed.assigned.push(url); }},
    history: {replaceState(_a, _b, url) { observed.cleaned = String(url); }},
    window: {open() { observed.opened++; return popup ? tab : null; }},
    state: {inventory: {apps: {apps: [item]}}},
    choose(section, id) { observed.selected = [section, id]; },
    notify(text) { observed.notifications.push(text); },
    async refresh() { observed.refreshes++; observed.notifications.push('Connected'); },
    async job() { observed.jobs++; return {id: 'job-test', state: 'running'}; },
    async api(route) {
      observed.requests.push(route);
      if (route.startsWith('/admin/dashboard/jobs/')) return {id: 'job-test', state: 'completed'};
      return {url: '/apps/mica-assistant/chat'};
    },
    async fetch(url) { observed.requests.push(url); const status = readiness.length > 1 ? readiness.shift() : readiness[0];
      return {status, ok: status === 200, body: {async cancel() {}}}; },
  };
  vm.createContext(context); vm.runInContext(implementation, context);
  return {context, observed, tab, item};
}

(async () => {
  let f = fixture();
  await f.context.openApp(f.item);
  assert.equal(f.tab.location.href, '/apps/mica-assistant/chat');
  assert.equal(f.tab.opener, null);
  assert.equal(f.observed.jobs, 0, 'Opening an existing app must not relaunch it');

  f = fixture({popup: false});
  await f.context.openApp(f.item);
  assert.deepEqual(f.observed.assigned, ['/apps/mica-assistant/chat']);
  assert.equal(f.observed.refreshes, 1, 'Refresh must not erase a popup fallback');

  f = fixture({readiness: [502, 503, 200]});
  f.item.running = false;
  await f.context.openApp(f.item, 'assistant-linux');
  assert.equal(f.observed.jobs, 1);
  assert.equal(f.observed.requests.filter(url => url === '/apps/mica-assistant/chat').length, 3);

  f = fixture();
  await f.context.openApp({id: 'mica-dictation', interfaces: ['tui'], running: false});
  assert.equal(f.observed.opened, 0);
  assert.equal(f.observed.jobs, 0, 'Do not launch a native UI as a hidden server daemon');
  assert.match(f.observed.notifications[0], /--tui/);

  f = fixture({query: '?app=mica-assistant'});
  await f.context.openLinkedApp();
  assert.equal(f.observed.opened, 0, 'CLI deep links must not need a popup gesture after login');
  assert.deepEqual(f.observed.assigned, ['/apps/mica-assistant/chat']);
  assert.equal(f.observed.cleaned, 'http://linux:8092/dashboard');

  f = fixture();
  f.context.api = async () => ({url: '//another-host/steal'});
  await assert.rejects(f.context.openApp(f.item), /Invalid app route/);
  assert.equal(f.tab.closed, true);
  console.log('Dashboard app opening: popup, fallback, readiness, native UI, deep link and route safety passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });

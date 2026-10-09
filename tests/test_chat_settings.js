const assert = require('node:assert/strict');
const {createWorkloadPicker, speedLabel} = require('../apps/mica-chat-settings.js');
global.Option = function(text, value) { return {text, value}; };
function nodes() {
  const select = {value: '', options: [], disabled: false, add(option) {this.options.push(option);}};
  Object.defineProperty(select, 'innerHTML', {set() {this.options = []; this.value = '';}});
  return {select, description: {}, active: {}, notice: {}, button: {}};
}
async function test() {
  assert.equal(speedLabel({available: true, decode_tokens_per_second: 106.34}), '106.3 tok/s · decode');
  for (const metrics of [null, {}, {available: true, decode_tokens_per_second: NaN},
      {available: true, decode_tokens_per_second: -1}, {available: false, decode_tokens_per_second: 20},
      {available: true, decode_tokens_per_second: '100'}])
    assert.equal(speedLabel(metrics), 'Decode speed unavailable');
  const ui = nodes(), requests = [], busyEvents = [];
  let current = 'spark', ready = true, chatBusy = false, confirmation = true, refreshed = 0;
  let failSwitch = false;
  const profiles = [
    {id: 'spark', description: 'Small assistant', required_ram_gib: 6, required_vram_gib: 0, models: [{id:'spark',quantization:'q4'}], activation_blockers:[]},
    {id: 'qwen', description: 'Qwen coder', required_ram_gib: 16, required_vram_gib: 15.5, models: [{id:'qwen27b',quantization:'iq3_xxs'}], activation_blockers:[]},
    {id: 'blocked', description: '<img src=x onerror=bad()>', required_ram_gib:32, required_vram_gib:0, models:[], activation_blockers:['Memory too small']},
  ];
  const picker = createWorkloadPicker({...ui, isBusy:()=>chatBusy, confirmSwitch:()=>confirmation,
    onBusy:value=>busyEvents.push(value), onActivated:async()=>{refreshed++}, delay:async()=>{ready=true},
    request:async(path, options)=>{
      requests.push([path,options]);
      if (options) {
        if (failSwitch) throw Error('Engine validation rejected');
        current=JSON.parse(options.body).profile; ready=false; return {profile:current};
      }
      return {active_workload:current,ready,data:profiles.map(row=>({...row,active:row.id===current,can_activate:ready&&!row.activation_blockers.length}))};
    }});
  await picker.refresh();
  assert.equal(ui.select.value,'spark'); assert.equal(ui.select.options.length,3);
  assert.equal(ui.button.disabled,true);
  ui.select.value='blocked';ui.select.onchange();
  assert.equal(ui.button.disabled,true); assert.match(ui.notice.textContent,/Memory too small/);
  assert.match(ui.description.textContent,/<img/); // Stored as text, never HTML.
  ui.select.value='qwen';ui.select.onchange();assert.equal(ui.button.disabled,false);
  chatBusy=true;assert.equal(await picker.activate(),false);chatBusy=false;
  confirmation=false;assert.equal(await picker.activate(),false);confirmation=true;
  assert.equal(requests.filter(([,options])=>options).length,0);
  assert.equal(await picker.activate(),true);
  assert.equal(current,'qwen');assert.deepEqual(busyEvents,[true,false]);assert.equal(refreshed,1);
  assert.match(ui.active.textContent,/Active: qwen/);assert.equal(ui.button.disabled,true);
  assert.equal(requests.find(([,options])=>options)[0],'/api/workloads/activate');
  ui.select.value='spark'; ui.select.onchange();failSwitch=true;
  await assert.rejects(picker.activate(),/validation rejected/);
  assert.equal(current,'qwen');assert.equal(ui.button.disabled,false);
  assert.deepEqual(busyEvents,[true,false,true,false]);
  current='hidden-legacy';ui.select.value='hidden-legacy';
  await picker.refresh();
  assert.equal(ui.select.value,'spark');
  assert.match(ui.active.textContent,/Active: hidden-legacy/);
  assert.equal(ui.button.disabled,false);
  console.log('Workload selector, confirmation, warmup, rejection recovery and speed labels passed');
}
test().catch(error=>{console.error(error);process.exitCode=1;});

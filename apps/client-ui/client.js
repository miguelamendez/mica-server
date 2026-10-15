'use strict';
// Device-local management; no framework, package manager or remote admin keys.
const $=id=>document.getElementById(id),state={key:'',view:'installed',data:null};
try{state.key=sessionStorage.getItem('micaClientKey')||''}catch{}
function node(tag,text){const n=document.createElement(tag);if(text!==undefined)n.textContent=text;return n}
function notice(text){$('notice').textContent=text}
function button(text,fn){const b=node('button',text);b.type='button';b.onclick=()=>Promise.resolve().then(fn).catch(e=>notice(e.message));return b}
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:{Authorization:'Bearer '+state.key,...(body?{'Content-Type':'application/json'}:{})},...(body?{body:JSON.stringify(body)}:{})});let data;try{data=await r.json()}catch{throw Error('Client response unavailable')}if(!r.ok)throw Error(data.error||'Client request failed');return data}
function action(body){return api('/api/action',body)}
function form(title,fields,submit,help=''){
  $('form-title').textContent=title;$('help').textContent=help;$('fields').replaceChildren();
  for(const f of fields){const label=node('label',f.label),input=node(f.options?'select':'input');input.name=f.name;
    if(f.options)for(const value of f.options){const o=node('option',value);o.value=value;input.append(o)}
    else input.type=f.secret?'password':'text';input.required=f.required!==false;if(f.value)input.value=f.value;label.append(input);$('fields').append(label)}
  $('form').onsubmit=async e=>{e.preventDefault();$('apply').disabled=true;try{await submit(Object.fromEntries(new FormData($('form'))));$('editor').close();await refresh()}catch(err){notice(err.message)}finally{$('apply').disabled=false}};$('editor').showModal();
}
async function refresh(){state.data=await api('/api/inventory');$('login').hidden=true;$('manager').hidden=false;const s=state.data.local_server;$('local-server').textContent='Local server: '+(s.running?'running at '+s.url:s.installed?'installed, stopped':'not installed');render()}
function commands(id){notice('Native app: run on this device:\nmica-client open '+id+' --accept-code\nOr use mica-client tui. A browser cannot open a native terminal automatically.')}
async function configure(app){
  form('Choose inference for '+app.name,[{name:'mode',label:'Where should inference run?',options:['Remote / existing connection','Local server','Configure later']}],async ({mode})=>{
    if(mode==='Configure later')return;
    // Wait until the current dialog submit finishes before opening another.
    setTimeout(()=>{if(mode==='Local server')localSetup(app);else bind(app)},0);
  });
}
function bind(app){const names=Object.keys(state.data.servers);if(!names.length){notice('Add a server in Servers first, then configure this app.');return}
  form('Connect '+app.name,[{name:'name',label:'Server connection',options:names}],values=>action({action:'app.bind',id:app.id,...values}),'Checks the selected workload supports the operations this app requires. No local models or engines are installed.');
}
async function localSetup(app){try{const manifest=await action({action:'app.show',id:app.id});const workloads=(manifest.registered_workloads||[]);if(!workloads.length)throw Error('This app has no bundled workload; connect to an existing server instead.');
  form('Prepare local inference',[{name:'workload',label:'Bundled workload (server validates hardware)',options:workloads}],async values=>{
    const missing=!state.data.local_server.installed;
    if(missing&&!confirm('Mica Server is absent. Build/install the official server? Requires Git, CMake, C++20 and Lua development headers. Compilation uses live-memory safeguards.'))return;
    const memory=manifest.local_setup_requirements?.[values.workload]||{};
    if(!confirm('Prepare engines/models and start local inference? Downloads may be many GB.\nWorkload requirements: '+(memory.required_ram_gib??'unspecified')+' GiB RAM, '+(memory.required_vram_gib??0)+' GiB dedicated VRAM.\nA new server uses these initial allocations; existing machine limits are preserved. Hardware and budget are validated before downloads. This does not change a running server’s default workload.'))return;
    notice('Preparing local inference. Keep this window open; this can take several minutes.');
    await action({action:'server.setup-local',id:app.id,...values,accept_downloads:true,install_server:missing});notice('Local inference connected.');
  },'Select the mac workload for Apple Silicon, cuda for supported NVIDIA hosts. Compatibility and memory are validated before engine setup.');
}catch(e){notice(e.message)}}
async function open(app){if(!app.interfaces?.includes('web')){commands(app.id);return}
  const tab=window.open('about:blank','_blank');if(tab)tab.opener=null;
  try{if(!app.running&&!confirm('Launch this trusted app on this device?')){tab?.close();return}
    const data=await action({action:'web.open',id:app.id,accept_code:true});
    if(!data.url.startsWith('/apps/')||data.url.startsWith('//'))throw Error('Invalid app URL');
    if(tab)tab.location.href=data.url;else location.assign(data.url);
  }catch(e){tab?.close();throw e}
}
function render(){const area=$('content');area.replaceChildren();for(const b of document.querySelectorAll('[data-view]'))b.classList.toggle('active',b.dataset.view===state.view);
  if(state.view==='servers'){
    area.append(button('Add server connection',()=>form('Connect to inference',[{name:'name',label:'Connection name'},{name:'url',label:'Server origin',value:'http://127.0.0.1:55000'},{name:'workload',label:'Workload ID'},{name:'api_key',label:'INFERENCE key (not admin)',secret:true}],v=>action({action:'connection.add',...v}),'The key is saved privately on this device. The workload is authenticated before saving.')));
    for(const [name,c]of Object.entries(state.data.servers)){const a=node('article');a.append(node('h2',name),node('p',c.url),node('p','Workload: '+c.workload));area.append(a)}return;
  }
  if(state.view==='discover')area.append(button('Add publisher repository',()=>form('Add app source',[{name:'name',label:'Source ID'},{name:'url',label:'GitHub/Hugging Face repository or HTTPS app/index URL'}],v=>action({action:'source.add',...v}),'Repositories may contain one app or an index of many. Code is not run during discovery.')));
  const apps=state.data.apps.apps.filter(a=>state.view==='discover'||a.installed);
  if(!apps.length)area.append(node('p','No apps installed. Open Discover to add one.'));
  for(const app of apps){const a=node('article'),row=node('div');row.className='actions';a.append(node('h2',app.name||app.id),node('p',app.description));const status=node('span',app.installed?(app.running?'Installed · running':'Installed · closed'):'Available');status.className='tag';a.append(status);
    row.append(button('Inspect',async()=>{const doc=await action({action:'app.show',id:app.id});notice(JSON.stringify({source:doc.source,permissions:doc.permissions,requirements:doc.requirements,launch:doc.launch,workloads:doc.workloads},null,2))}));
    if(!app.installed)row.append(button('Install on this device',async()=>{const doc=await action({action:'app.show',id:app.id});if(!confirm('Install '+app.name+'?\nPublisher: '+doc.source.repository+'\nPermissions: '+doc.permissions.join(', ')+'\nDownloaded code is not sandboxed.'))return;await action({action:'app.install',id:app.id,accept_code:true});await refresh();await configure(app)}));
    else row.append(button('Open',()=>open(app)),button('Configure inference',()=>configure(app)),button('Close',async()=>{await action({action:'app.stop',id:app.id});await refresh()}),button('Uninstall',async()=>{if(confirm('Unregister '+app.name+'? Files are retained for recovery.')){await action({action:'app.uninstall',id:app.id});await refresh()}}));
    a.append(row);if(app.connections)a.append(node('p','Connections: '+Object.keys(app.connections).join(', ')));area.append(a);
  }
}
for(const b of document.querySelectorAll('[data-view]'))b.onclick=()=>{state.view=b.dataset.view;render()};
$('cancel').onclick=()=>$('editor').close();$('refresh').onclick=()=>refresh().catch(e=>notice(e.message));
$('login').onsubmit=async e=>{e.preventDefault();state.key=$('key').value.trim();try{await refresh();sessionStorage.setItem('micaClientKey',state.key);$('key').value='';await linkedApp()}catch(err){notice(err.message)}};
$('disconnect').onclick=async()=>{try{await api('/api/logout',{})}catch{}state.key='';sessionStorage.removeItem('micaClientKey');$('manager').hidden=true;$('login').hidden=false;notice('Disconnected')};
async function linkedApp(){const id=new URLSearchParams(location.search).get('app');if(!id)return;const app=state.data.apps.apps.find(a=>a.id===id&&a.installed);if(!app)throw Error('App is not installed on this device');if(!app.interfaces?.includes('web')){commands(id);return}if(!app.running&&!confirm('Launch this trusted app?'))return;const result=await action({action:'web.open',id,accept_code:true});if(!result.url.startsWith('/apps/')||result.url.startsWith('//'))throw Error('Invalid app route');location.assign(result.url)}
if(state.key)refresh().then(linkedApp).catch(e=>notice(e.message));

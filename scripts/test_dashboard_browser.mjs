// Optional offline QA: installed Chrome + Node's built-in CDP WebSocket.
// No browser packages, npm install, or application runtime dependency.
import {spawn, spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, writeFileSync, existsSync, rmSync, mkdirSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const args=Object.fromEntries(process.argv.slice(2).reduce((pairs,item,index,all)=>{
  if(index%2===0)pairs.push([item.replace(/^--/,''),all[index+1]]);return pairs;
},[]));
for(const field of ['url','chrome','ssh-target','ssh-key'])if(!args[field])throw Error('Required --'+field);
const ssh=['-o','BatchMode=yes','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-i',args['ssh-key']];
if(args['ssh-host-alias'])ssh.push('-o','HostKeyAlias='+args['ssh-host-alias']);
const output=resolve(args.output||join(tmpdir(),'mica-dashboard-browser-results'));mkdirSync(output,{recursive:true});
const profile=mkdtempSync(join(tmpdir(),'mica-dashboard-browser-'));
let chrome,ws,adminKey='',counter=0;const pending=new Map(),runtimeErrors=[];
const pause=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,timeout=20000){const end=Date.now()+timeout;while(Date.now()<end){try{if(await fn())return}catch{}await pause(150)}throw Error('Browser check timed out')}
function send(method,params={}){const id=++counter;return new Promise((resolve,reject)=>{pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));setTimeout(()=>{if(pending.delete(id))reject(Error('CDP command timed out'))},20000).unref()})}
async function evaluate(expression){const result=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(result.exceptionDetails)throw Error('Browser JavaScript evaluation failed');return result.result.value}
async function click(section){await evaluate(`document.querySelector('[data-section="${section}"]').click()`);await until(()=>evaluate(`document.getElementById('title').textContent===${JSON.stringify(section[0].toUpperCase()+section.slice(1))}`))}
try{
  const credential=spawnSync('/usr/bin/ssh',[...ssh,args['ssh-target'],'cat .mica/secrets/admin-api-key'],{encoding:'utf8',timeout:15000});
  if(credential.status!==0)throw Error('Cannot read the server admin credential through trusted SSH');
  adminKey=credential.stdout.trim();if(adminKey.length<16)throw Error('Missing admin credential');
  chrome=spawn(args.chrome,['--headless=new','--disable-gpu','--disable-background-networking','--disable-extensions',
    '--no-first-run','--no-default-browser-check','--renderer-process-limit=1','--window-size=1280,900',
    '--disable-features=OptimizationGuideOnDeviceModel,OnDeviceModelExecution,PassageEmbeddings,HistoryEmbeddings,OptimizationHints',
    '--remote-debugging-port=0','--user-data-dir='+profile,'about:blank'],{stdio:'ignore'});
  await until(()=>existsSync(join(profile,'DevToolsActivePort')));
  const port=readFileSync(join(profile,'DevToolsActivePort'),'utf8').split('\n')[0];
  const pages=await fetch('http://127.0.0.1:'+port+'/json/list').then(r=>r.json());
  const page=pages.find(p=>p.type==='page');if(!page)throw Error('Installed Chrome has no test page');
  for(const other of pages.filter(p=>p.type==='page'&&p.id!==page.id))
    await fetch('http://127.0.0.1:'+port+'/json/close/'+other.id);
  ws=new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=()=>reject(Error('Cannot connect to installed Chrome'))});
  ws.onmessage=event=>{const message=JSON.parse(event.data);if(message.method==='Runtime.exceptionThrown')runtimeErrors.push('Uncaught browser exception');if(message.id&&pending.has(message.id)){const item=pending.get(message.id);pending.delete(message.id);message.error?item.reject(Error('CDP operation failed')):item.resolve(message.result)}};
  await send('Runtime.enable');await send('Page.enable');
  await send('Page.navigate',{url:args.url});
  await until(()=>evaluate("document.getElementById('login')?.open"));
  await evaluate(`document.getElementById('key').value=${JSON.stringify(adminKey)};document.getElementById('login-form').requestSubmit()`);
  await until(()=>evaluate("!document.getElementById('login').open&&document.getElementById('connection').textContent.includes('Connected')"));
  const checks=[];checks.push('Admin login and host inventory');
  for(const section of ['workloads','models','engines','apps','endpoints','settings']){await click(section);if(!await evaluate("document.getElementById('view').textContent.trim().length>0"))throw Error('Empty '+section+' view');checks.push(section+' navigation/rendering')}
  await click('workloads');
  await evaluate("[...document.querySelectorAll('#view button')].find(b=>b.textContent==='Edit YAML').click()");
  await until(()=>evaluate("document.getElementById('editor').open&&document.querySelector('#editor textarea').value.includes('schema:')"));
  await evaluate("document.getElementById('editor-cancel').click()");checks.push('Workload YAML editor opens without mutating configuration');
  await click('models');await evaluate("const select=document.querySelector('#view select');select.value='ASR';select.dispatchEvent(new Event('change'))");
  if(!await evaluate("document.querySelector('#view .list').textContent.includes('ASR')"))throw Error('ASR category filter failed');checks.push('Model category filter');
  await click('settings');if(!await evaluate("document.getElementById('view').textContent.includes('Rotate admin key')&&document.getElementById('view').textContent.includes('Rotate inference key')"))throw Error('Credential controls missing');checks.push('Separate credential controls');
  await click('endpoints');await evaluate("document.querySelector('#view form').requestSubmit()");
  await until(()=>evaluate("[...document.querySelectorAll('#view pre')].some(p=>p.textContent.includes('\\\"status\\\": \\\"ok\\\"'))"));checks.push('Actual health endpoint execution');
  await evaluate("document.getElementById('theme').value='latte';document.getElementById('theme').dispatchEvent(new Event('change'))");
  if(!await evaluate("document.documentElement.dataset.theme==='latte'"))throw Error('Theme switch failed');checks.push('Theme switch');
  await evaluate("document.getElementById('theme').value='mocha';document.getElementById('theme').dispatchEvent(new Event('change'))");
  await click('server');
  const screenshot=await send('Page.captureScreenshot',{format:'png'});writeFileSync(join(output,'dashboard-desktop.png'),Buffer.from(screenshot.data,'base64'));
  await send('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  await pause(250);const mobile=await send('Page.captureScreenshot',{format:'png'});writeFileSync(join(output,'dashboard-mobile.png'),Buffer.from(mobile.data,'base64'));checks.push('Desktop/mobile screenshots');
  if(runtimeErrors.length)throw Error('Uncaught browser runtime errors');
  const storage=await evaluate("Object.keys(localStorage)");if(storage.some(k=>/key|token|secret/i.test(k)))throw Error('Credential unexpectedly persisted in browser storage');checks.push('No persisted browser credential');
  console.log(JSON.stringify({passed:true,checks,screenshots:output},null,2));
}catch(error){console.error('Dashboard browser QA failed: '+error.message);process.exitCode=1}
finally{adminKey='';if(ws)ws.close();if(chrome){chrome.kill('SIGTERM');await pause(500);if(chrome.exitCode===null)chrome.kill('SIGKILL')}if(profile.startsWith(join(tmpdir(),'mica-dashboard-browser-')))rmSync(profile,{recursive:true,force:true})}

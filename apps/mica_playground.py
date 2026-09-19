#!/usr/bin/env python3
"""Dependency-free local voice and multimodal playground for mica-server."""

from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ASSET_DIR = Path(__file__).resolve().parent


CHAT_HTML = r"""<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Mica agent chat</title>
<style>
:root{color-scheme:dark;--rosewater:#f5e0dc;--flamingo:#f2cdcd;--pink:#f5c2e7;--mauve:#cba6f7;--red:#f38ba8;--peach:#fab387;--yellow:#f9e2af;--green:#a6e3a1;--teal:#94e2d5;--sky:#89dceb;--sapphire:#74c7ec;--blue:#89b4fa;--lavender:#b4befe;--text:#cdd6f4;--subtext:#a6adc8;--overlay:#6c7086;--surface1:#45475a;--surface0:#313244;--base:#1e1e2e;--mantle:#181825;--crust:#11111b;--line:#313244;--panel:#181825;--muted:#a6adc8}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 16% -12%,#313244 0,transparent 34%),radial-gradient(circle at 88% -4%,#30263f 0,transparent 28%),var(--crust);color:var(--text);font:15px Inter,ui-sans-serif,system-ui}
.app{position:relative;max-width:1040px;height:calc(100vh - 24px);margin:12px auto;display:grid;grid-template-rows:auto 1fr auto;background:#1e1e2ee8;border:1px solid var(--surface0);border-radius:26px;box-shadow:0 24px 90px #0009;overflow:hidden}
.top{padding:12px 18px;border-bottom:1px solid var(--line);display:flex;gap:12px;align-items:center;background:#181825ed;backdrop-filter:blur(18px)}
.brand{display:flex;align-items:center;gap:11px}.logo{width:42px;height:42px;padding:5px;border:1px solid #b4befe38;border-radius:14px;background:linear-gradient(145deg,#313244,#252536);box-shadow:inset 0 1px #ffffff0d,0 5px 14px #11111b80}.title strong{display:block;font-size:18px;letter-spacing:.01em;color:var(--text)}.title small{display:block;color:var(--subtext);font-size:11px}
.pill{color:var(--green);background:#a6e3a114;padding:6px 10px;border:1px solid #a6e3a145;border-radius:999px;font-size:12px}.session{color:var(--overlay);font:11px ui-monospace,monospace}#limits{max-width:250px;padding:5px 9px;border:1px solid #89dceb32;border-radius:999px;color:var(--sky);background:#89dceb0d;font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chat{overflow:auto;padding:26px;display:flex;flex-direction:column}.message-row{display:flex;align-items:flex-end;gap:10px;max-width:86%;margin:9px 0}.message-row.user{align-self:flex-end;flex-direction:row-reverse}.message-row.assistant{align-self:flex-start}.avatar{width:35px;height:35px;flex:0 0 35px;border-radius:50%;display:grid;place-items:center;border:1px solid var(--surface1);box-shadow:0 5px 14px #11111b66}.assistant-avatar{background:linear-gradient(145deg,#45475a,#313244)}.assistant-avatar img{width:25px;height:25px}.user-avatar{background:linear-gradient(145deg,var(--pink),var(--mauve));border-color:#f5c2e755}.user-avatar svg{width:19px;height:19px;fill:none;stroke:var(--crust);stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}.message{min-width:52px;padding:13px 16px;white-space:pre-wrap;line-height:1.48;box-shadow:0 7px 24px #11111b55}.user .message{background:#45475a;color:var(--text);border:1px solid #585b70;border-radius:22px 22px 7px 22px}.assistant .message{background:var(--mantle);border:1px solid var(--surface0);border-radius:22px 22px 22px 7px}.meta{display:inline-flex;width:fit-content;max-width:100%;align-items:center;gap:5px;margin-top:10px;padding:5px 9px;border:1px solid #89b4fa30;border-radius:999px;background:#89b4fa0d;color:var(--blue);font-size:11px;white-space:normal}.agent-activity{align-self:flex-start;display:flex;align-items:center;gap:8px;margin:4px 0 4px 45px;padding:7px 11px;border:1px solid #f5c2e738;border-radius:999px;background:#f5c2e70d;color:var(--pink);font-size:12px;box-shadow:0 5px 18px #11111b44}.agent-activity::before{content:'';width:7px;height:7px;border-radius:50%;background:currentColor;box-shadow:0 0 0 4px #f5c2e712;animation:pulse 1.2s ease-in-out infinite}@keyframes pulse{50%{opacity:.35;transform:scale(.75)}}
.composer{border-top:1px solid var(--line);padding:12px 15px 14px;background:#181825ed;backdrop-filter:blur(18px)}.input-shell{border:1px solid var(--surface1);border-radius:22px;background:var(--base);box-shadow:inset 0 1px 2px #11111b80,0 8px 30px #11111b35;overflow:hidden;transition:.15s border-color,.15s box-shadow}.input-shell:focus-within{border-color:var(--lavender);box-shadow:0 0 0 3px #b4befe1c,0 8px 30px #11111b45}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.grow{flex:1}textarea,select,button,input{font:inherit}textarea{display:block;width:100%;min-height:66px;max-height:220px;resize:none;border:0;padding:14px 15px 6px;background:transparent;color:var(--text);outline:none}textarea::placeholder{color:var(--overlay)}button,.button{border:1px solid var(--surface1);border-radius:14px;padding:9px 12px;background:var(--surface0);color:var(--text);cursor:pointer;transition:.15s transform,.15s background,.15s border-color}button:hover,.button:hover{background:var(--surface1);border-color:var(--overlay);transform:translateY(-1px)}button:disabled{opacity:.55;cursor:wait}.icon-button{width:38px;height:38px;display:inline-grid;place-items:center;flex:0 0 38px;padding:0;border-radius:13px}.icon-button svg{width:19px;height:19px;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}.primary{background:var(--mauve);border-color:var(--mauve);color:var(--crust);font-weight:700;box-shadow:0 6px 18px #cba6f72b}.primary:hover{background:var(--lavender);border-color:var(--lavender)}.quiet{padding:7px 10px;font-size:12px}.recording{background:#f38ba826;border-color:var(--red);color:var(--red)}.composer-tools{padding:6px 7px 7px}.shortcut-help{color:var(--overlay);font-size:10px;white-space:nowrap}.attachments{display:flex;gap:6px;flex-wrap:wrap;margin:0;padding:0 10px 7px}.attachments:empty{display:none}.chip{background:#89b4fa14;border:1px solid #89b4fa45;color:var(--blue);border-radius:999px;padding:5px 9px;font-size:12px}.status{display:flex;width:fit-content;max-width:100%;align-items:center;gap:7px;margin-top:9px;padding:6px 10px;border:1px solid #94e2d52b;border-radius:999px;background:#94e2d50d;color:var(--teal);font-size:12px;line-height:1.3}.status::before{content:'';width:7px;height:7px;flex:0 0 7px;border-radius:50%;background:currentColor;box-shadow:0 0 0 3px #94e2d516}fieldset{margin:12px 0 0;padding:12px;border:1px solid var(--surface0);border-radius:14px;display:grid;grid-template-columns:1fr;gap:9px}label{display:flex;gap:7px;align-items:center}select{min-width:0;flex:1;background:var(--base);color:var(--text);border:1px solid var(--surface1);border-radius:10px;padding:7px}input[type=checkbox]{accent-color:var(--mauve)}audio{width:100%;margin-top:10px}.hidden{display:none}
.sidebar{position:absolute;z-index:30;inset:0 auto 0 0;width:min(310px,88%);padding:16px;display:flex;flex-direction:column;background:#181825fa;border-right:1px solid var(--surface0);box-shadow:18px 0 60px #11111baa;transform:translateX(-105%);transition:transform .2s ease}.sidebar.open{transform:translateX(0)}.scrim{position:absolute;z-index:25;inset:0;background:#11111b99;backdrop-filter:blur(2px);opacity:0;pointer-events:none;transition:opacity .2s}.scrim.open{opacity:1;pointer-events:auto}.sidebar-head{display:flex;align-items:center;gap:10px;margin-bottom:16px}.sidebar-head strong{font-size:17px}.new-chat{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;border-color:#cba6f74d;background:#cba6f714;color:var(--mauve)}.new-chat svg{width:17px;height:17px;fill:none;stroke:currentColor;stroke-width:2}.history-label{margin:18px 4px 8px;color:var(--overlay);font-size:10px;font-weight:700;letter-spacing:.12em;text-transform:uppercase}.session-list{display:flex;flex:1;min-height:0;overflow:auto;flex-direction:column;gap:5px}.session-item{display:block;width:100%;padding:10px 11px;text-align:left;border-color:transparent;background:transparent}.session-item:hover{background:var(--surface0)}.session-item.active{border-color:#b4befe42;background:#b4befe10}.session-title{display:block;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.session-time{display:block;margin-top:3px;color:var(--overlay);font-size:10px}.sidebar-actions{display:grid;grid-template-columns:1fr 1fr;gap:7px;padding-top:12px;border-top:1px solid var(--surface0)}
.settings-panel{position:absolute;z-index:20;top:68px;right:14px;width:min(390px,calc(100% - 28px));max-height:calc(100% - 84px);overflow:auto;padding:15px;border:1px solid var(--surface1);border-radius:18px;background:#181825f7;box-shadow:0 18px 60px #11111bcc;transform:translateY(-8px) scale(.98);opacity:0;pointer-events:none;transition:.15s}.settings-panel.open{transform:none;opacity:1;pointer-events:auto}.settings-title{display:flex;align-items:center;justify-content:space-between}.settings-title strong{font-size:16px}.setting-row{justify-content:space-between;margin-top:13px;padding:10px 11px;border-radius:13px;background:var(--base);border:1px solid var(--surface0)}.setting-row small{color:var(--overlay)}.settings-panel #limits{display:block;max-width:none;margin-top:10px;white-space:normal}.top .icon-button{border-color:transparent;background:transparent}.top .icon-button:hover{border-color:var(--surface1);background:var(--surface0)}
.media-strip{display:flex;gap:7px;max-width:100%;margin-top:10px;padding-bottom:2px;overflow-x:auto;scrollbar-width:thin}.media-tile{width:64px;min-width:64px;padding:5px;border:1px solid var(--surface1);border-radius:13px;background:#313244a8;color:var(--subtext);text-decoration:none;cursor:pointer;transition:.15s transform,.15s border-color}.media-tile:hover{border-color:var(--lavender);transform:translateY(-1px)}.media-thumb{width:52px;height:42px;display:grid;place-items:center;border-radius:9px;background:var(--base);overflow:hidden}.media-thumb img{width:100%;height:100%;object-fit:cover}.media-thumb svg{width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}.media-tile.image{color:var(--green)}.media-tile.video{color:var(--mauve)}.media-tile.audio{color:var(--peach)}.media-tile.document{color:var(--blue)}.media-name{display:block;margin-top:4px;overflow:hidden;color:var(--subtext);font-size:9px;text-overflow:ellipsis;white-space:nowrap}.attachments .media-strip{margin:0}.attachments .media-tile{width:58px;min-width:58px}.attachments .media-thumb{width:46px;height:36px}.voice-config,.api-key-config{margin-top:12px;padding:11px;border:1px solid var(--surface0);border-radius:14px;background:var(--base)}.voice-config>label{justify-content:space-between}.voice-controls{margin-top:10px;padding-top:10px;border-top:1px solid var(--surface0)}.voice-actions{display:flex;gap:6px;flex-wrap:wrap}.voice-actions button,.voice-actions .button{padding:7px 9px;font-size:11px}.voice-file{margin:8px 0 0;color:var(--peach);font-size:11px}.voice-config audio{height:34px;margin:8px 0}.voice-config .transcript{display:block;margin-top:8px;color:var(--subtext);font-size:11px}.voice-config input[type=text],.api-key-config input{display:block;width:100%;margin-top:5px;padding:8px 9px;border:1px solid var(--surface1);border-radius:10px;background:var(--mantle);color:var(--text);outline:none}.voice-config input[type=text]:focus,.api-key-config input:focus{border-color:var(--lavender)}.voice-note{display:block;margin-top:7px;color:var(--overlay);font-size:10px;line-height:1.4}.api-key-config label{display:block;color:var(--subtext);font-size:11px}.api-key-actions{display:flex;align-items:center;gap:8px;margin-top:8px}.api-key-actions small{color:var(--overlay)}
.message.media-only{padding:8px 10px}.message.media-only .media-strip{margin-top:0}.message-copy:empty{display:none}.media-tile.audio.voice-bubble{position:relative;width:210px;min-width:210px;height:54px;padding:7px 8px;display:grid;grid-template-columns:34px 1fr 25px;grid-template-rows:1fr 4px;gap:3px 8px;align-items:center;cursor:default}.voice-play{grid-row:1/3;width:34px;height:34px;padding:0;display:grid;place-items:center;border:0;border-radius:50%;background:#fab38720;color:var(--peach)}.voice-play:hover{background:#fab38735;border:0;transform:none}.voice-play svg,.media-open svg{width:17px;height:17px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}.voice-details{min-width:0}.voice-details .media-name{margin:0;font-size:10px;color:var(--text)}.voice-time{display:block;margin-top:2px;color:var(--overlay);font-size:9px}.voice-progress{height:4px;overflow:hidden;border-radius:999px;background:var(--surface1)}.voice-progress span{display:block;width:0;height:100%;border-radius:inherit;background:var(--peach)}.media-open{grid-column:3;grid-row:1/3;width:25px;height:25px;display:grid;place-items:center;border-radius:8px;color:var(--subtext);text-decoration:none}.media-open:hover{background:var(--surface1);color:var(--text)}.attachments .media-tile.audio.voice-bubble{width:190px;min-width:190px}.markdown{white-space:normal}.markdown p,.markdown ul,.markdown ol,.markdown blockquote,.markdown pre,.markdown table{margin:.15em 0 .8em}.markdown>:last-child{margin-bottom:0}.markdown h1,.markdown h2,.markdown h3,.markdown h4{margin:.15em 0 .55em;line-height:1.25;color:var(--rosewater)}.markdown h1{font-size:1.35em}.markdown h2{font-size:1.2em}.markdown h3,.markdown h4{font-size:1.06em}.markdown ul,.markdown ol{padding-left:1.35em}.markdown li+li{margin-top:.25em}.markdown blockquote{padding:.25em .8em;border-left:3px solid var(--mauve);color:var(--subtext);background:#cba6f709;border-radius:0 9px 9px 0}.markdown code{padding:.12em .35em;border:1px solid var(--surface1);border-radius:6px;background:var(--base);color:var(--peach);font:12px ui-monospace,SFMono-Regular,Menlo,monospace}.markdown pre{max-width:min(720px,70vw);padding:11px 12px;overflow:auto;border:1px solid var(--surface1);border-radius:12px;background:var(--crust)}.markdown pre code{padding:0;border:0;background:transparent;color:var(--text);white-space:pre}.markdown a{color:var(--sapphire);text-decoration-thickness:1px;text-underline-offset:2px}.markdown table{display:block;max-width:min(720px,70vw);overflow:auto;border-collapse:collapse}.markdown th,.markdown td{padding:6px 9px;border:1px solid var(--surface1);text-align:left;white-space:nowrap}.markdown th{background:var(--surface0);color:var(--lavender)}.markdown hr{margin:.85em 0;border:0;border-top:1px solid var(--surface1)}
@media(max-width:680px){body{background:var(--base)}.app{height:100vh;margin:0;border:0;border-radius:0}.session,.top #limits{display:none}.top{padding-inline:10px}.chat{padding:14px}.message-row{max-width:96%}.avatar{width:31px;height:31px;flex-basis:31px}.assistant-avatar img{width:22px;height:22px}.shortcut-help{display:none}.title small{display:none}}
</style>
<div class="app">
<aside class="sidebar" id="sidebar" aria-label="Chat history">
  <div class="sidebar-head"><img class="logo" src="/mica-logo.svg" alt=""><strong>Conversations</strong><span class="grow"></span><button class="icon-button" id="closeMenu" aria-label="Close chat history" title="Close chat history"><svg viewBox="0 0 24 24"><path d="m7 7 10 10M17 7 7 17"/></svg></button></div>
  <button class="new-chat" id="newChat"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg>New conversation</button>
  <div class="history-label">Recent</div><div class="session-list" id="sessionList"></div>
  <div class="sidebar-actions"><button class="quiet" id="exportSession">Export current</button><label class="button quiet">Import archive<input class="hidden" id="importSession" type="file" accept=".zip,application/zip"></label></div>
</aside><div class="scrim" id="scrim"></div>
<div class="top">
  <button class="icon-button" id="menuButton" aria-label="Open chat history" title="Chat history"><svg viewBox="0 0 24 24"><path d="M4 7h16M4 12h16M4 17h11"/></svg></button>
  <div class="brand"><img class="logo" src="/mica-logo.svg" alt=""><div class="title"><strong>Mica</strong><small>local multimodal agent</small></div></div>
  <span class="pill" id="agentState">ready</span><span class="session" id="sessionLabel"></span><span class="grow"></span>
  <button class="icon-button" id="settingsButton" aria-label="Open settings" title="Settings"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.09A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.09A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.09A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.15.38.36.72.65.98.3.27.7.41 1.1.42H21v4h-.09A1.7 1.7 0 0 0 19.4 15Z"/></svg></button>
</div>
<section class="settings-panel" id="settingsPanel" aria-label="Chat settings">
  <div class="settings-title"><strong>Chat settings</strong><button class="icon-button" id="closeSettings" aria-label="Close settings"><svg viewBox="0 0 24 24"><path d="m7 7 10 10M17 7 7 17"/></svg></button></div>
  <div class="api-key-config">
    <label><strong>Mica API key</strong><br><small>Kept only in this browser tab</small><input id="apiKey" type="password" autocomplete="off" spellcheck="false" placeholder="Required when server authentication is enabled"></label>
    <div class="api-key-actions"><button id="saveApiKey" type="button">Apply key</button><button id="clearApiKey" type="button">Clear</button><small id="apiKeyState">Not set</small></div>
  </div>
  <label class="setting-row"><span><strong>Stream responses</strong><br><small>Show text and audio as they arrive</small></span><input id="streamMode" type="checkbox" checked></label>
  <div class="voice-config">
    <label><span><strong>TTS reply voice</strong><br><small>Saved across conversations</small></span><select id="ttsVoiceMode"><option value="default">Built-in default</option><option value="reference">Custom reference</option></select></label>
    <div class="voice-controls hidden" id="ttsVoiceControls">
      <div class="voice-actions"><label class="button">Upload audio<input class="hidden" id="ttsVoiceUpload" type="file" accept="audio/*"></label><button id="ttsVoiceRecord">Record sample</button><button id="ttsVoiceTranscribe">Auto-transcribe</button><button id="ttsVoiceRemove">Remove</button></div>
      <div class="voice-file" id="ttsVoiceName">No reference selected</div><audio class="hidden" id="ttsVoicePreview" controls></audio>
      <label class="transcript">Exact reference transcript<input id="ttsVoiceText" type="text" placeholder="What is spoken in the reference clip"></label>
      <small class="voice-note">Use 5–15 seconds of clean speech. Audio8 conditions on the audio and its exact transcript together.</small>
    </div>
  </div>
  <span id="limits"></span>
  <fieldset>
    <label>LLM <select id="llm"></select><select id="llmQuant"></select></label>
    <label>VLM <select id="vlm"></select><select id="vlmQuant"></select></label>
    <label>ASR <select id="asr"></select><select id="asrQuant"></select></label>
    <label>TTS <select id="tts"></select><select id="ttsQuant"></select></label>
  </fieldset>
</section>
<main class="chat" id="chat"></main>
<section class="composer"><div class="input-shell">
  <textarea id="text" placeholder="Message Mica…"></textarea><div class="attachments" id="attachments"></div>
  <div class="row composer-tools">
    <label class="button icon-button" aria-label="Attach files" title="Attach files"><svg viewBox="0 0 24 24"><path d="m20.5 11.5-8.8 8.8a6 6 0 0 1-8.5-8.5l9.2-9.2a4 4 0 0 1 5.7 5.7l-9.2 9.2a2 2 0 0 1-2.8-2.8l8.5-8.5"/></svg><input class="hidden" id="files" type="file" multiple accept="image/*,video/*,.pdf,.txt,.md,.json,.csv,.yaml,.yml,.log,.html,.xml"></label>
    <button class="icon-button" id="record" aria-label="Record voice" title="Record voice"><svg viewBox="0 0 24 24"><rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v3M9 21h6"/></svg></button>
    <button class="icon-button hidden" id="clearVoice" aria-label="Remove voice recording" title="Remove voice recording"><svg viewBox="0 0 24 24"><path d="m7 7 10 10M17 7 7 17"/></svg></button>
    <span class="shortcut-help" id="shortcutHelp"></span><span class="grow"></span>
    <button class="icon-button primary" id="send" aria-label="Send message" title="Send message"><svg viewBox="0 0 24 24"><path d="m4 4 17 8-17 8 3-8-3-8Z"/><path d="M7 12h14"/></svg></button>
  </div>
</div><div class="status" id="status">Loading models…</div></section>
</div>
<script>
const $=id=>document.getElementById(id);let sessionId=localStorage.micaSessionId||crypto.randomUUID();localStorage.micaSessionId=sessionId;
let rows=[],selectedFiles=[],voiceBlob=null,ctx,stream,node,chunks=[],recording=false;
let ttsVoiceBlob=null,ttsVoiceFilename='',ttsVoiceCtx=null,ttsVoiceStream=null,ttsVoiceNode=null,ttsVoiceChunks=[],ttsVoiceRecording=false;
let apiKey=sessionStorage.getItem('micaApiKey')||'';
const platform=navigator.userAgentData?.platform||navigator.platform||'',isMac=/mac/i.test(platform),historyKey='micaSessionHistory';
const status=x=>$('status').textContent=x,baseId=x=>x.id.includes('@')?x.id.slice(0,x.id.lastIndexOf('@')):x.id;
function apiFetch(url,options={}){let headers=new Headers(options.headers||{});if(apiKey)headers.set('X-Mica-API-Key',apiKey);return fetch(url,{...options,headers})}
function updateApiKeyUi(){if(!$('apiKey'))return;$('apiKey').value=apiKey;$('apiKeyState').textContent=apiKey?'Set for this tab':'Not set'}
async function requireOk(response){if(response.ok)return response;let result;try{result=await response.clone().json()}catch{result=null}let fallback=(await response.text())||`HTTP ${response.status}`,message=result?.error?.message||result?.message||fallback;if(response.status===401){openSettings(true);$('apiKey').focus()}throw Error(message)}
function sessionHistory(){try{let value=JSON.parse(localStorage.getItem(historyKey)||'[]');return Array.isArray(value)?value:[]}catch{return[]}}
function storeHistory(value){localStorage.setItem(historyKey,JSON.stringify(value.slice(0,50)))}
function ensureSession(title=''){let items=sessionHistory(),item=items.find(x=>x.id===sessionId),now=Date.now();if(!item){item={id:sessionId,title:'New conversation',updatedAt:now};items.push(item)}if(title&&item.title==='New conversation')item.title=title.replace(/\s+/g,' ').trim().slice(0,52)||item.title;if(title)item.updatedAt=now;items.sort((a,b)=>(b.updatedAt||0)-(a.updatedAt||0));storeHistory(items);renderHistory()}
function renderHistory(){let list=$('sessionList');if(!list)return;list.innerHTML='';sessionHistory().forEach(item=>{let button=document.createElement('button');button.className='session-item'+(item.id===sessionId?' active':'');let title=document.createElement('span');title.className='session-title';title.textContent=item.title||'Conversation';let time=document.createElement('span');time.className='session-time';time.textContent=new Date(item.updatedAt||Date.now()).toLocaleString([], {dateStyle:'medium',timeStyle:'short'});button.append(title,time);button.onclick=()=>selectSession(item.id);list.appendChild(button)})}
function openSidebar(open=true){$('sidebar').classList.toggle('open',open);$('scrim').classList.toggle('open',open)}
function openSettings(open=true){$('settingsPanel').classList.toggle('open',open)}
async function selectSession(id){if($('send').disabled)return status('Wait for the current response before changing conversations.');sessionId=id;localStorage.micaSessionId=id;renderHistory();openSidebar(false);await loadSession();$('text').focus()}
function newSession(){if($('send').disabled)return status('Wait for the current response before starting another conversation.');sessionId=crypto.randomUUID();localStorage.micaSessionId=sessionId;ensureSession();renderSession({messages:[]});openSidebar(false);status('New conversation ready.');$('text').focus()}
function quants(prefix,preferred){let base=$(prefix).value,q=$(prefix+'Quant'),found=rows.filter(x=>baseId(x)===base);q.innerHTML='';found.forEach(x=>q.add(new Option(`${x.quantization.toUpperCase()} · ${x.backend.toUpperCase()} · ${x.state}`,x.id)));let i=found.findIndex(x=>x.quantization===preferred);if(i>=0)q.selectedIndex=i}
function capability(prefix,cap,preferred){let s=$(prefix);s.innerHTML='';[...new Set(rows.filter(x=>x.capability===cap).map(baseId))].forEach(x=>s.add(new Option(x,x)));s.onchange=()=>quants(prefix,preferred);quants(prefix,preferred)}
const model=p=>$(p+'Quant').value;
const previewUrls=new WeakMap();
let activeMediaAudio=null;
function kindOf(item){let type=(item.content_type||item.type||'').toLowerCase(),name=(item.name||'').toLowerCase();if(item.kind&&item.kind!=='file')return item.kind;if(type.startsWith('image/')||/\.(png|jpe?g|gif|webp|heic)$/.test(name))return'image';if(type.startsWith('video/')||/\.(mp4|mov|webm|mkv)$/.test(name))return'video';if(type.startsWith('audio/')||/\.(wav|mp3|m4a|aac|flac|ogg)$/.test(name))return'audio';return'document'}
function mediaIcon(kind){if(kind==='image')return'<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="3"/><circle cx="9" cy="9" r="2"/><path d="m4 17 5-5 4 4 2-2 5 4"/></svg>';if(kind==='video')return'<svg viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="14" rx="3"/><path d="m10 9 5 3-5 3V9Z"/></svg>';if(kind==='audio')return'<svg viewBox="0 0 24 24"><path d="M4 14v-4M8 17V7M12 20V4M16 17V7M20 14v-4"/></svg>';return'<svg viewBox="0 0 24 24"><path d="M6 3h8l4 4v14H6V3Z"/><path d="M14 3v5h5M9 13h6M9 17h6"/></svg>'}
function objectUrl(value){if(!value||typeof value!=='object')return'';if(!previewUrls.has(value))previewUrls.set(value,URL.createObjectURL(value));return previewUrls.get(value)}
function clock(seconds){if(!Number.isFinite(seconds))return'voice note';let whole=Math.max(0,Math.floor(seconds));return`${Math.floor(whole/60)}:${String(whole%60).padStart(2,'0')}`}
function playIcon(playing){return playing?'<svg viewBox="0 0 24 24"><path d="M8 5v14M16 5v14"/></svg>':'<svg viewBox="0 0 24 24"><path d="m9 6 9 6-9 6V6Z"/></svg>'}
function audioTile(item,url){let tile=document.createElement('div');tile.className='media-tile audio voice-bubble';tile.title=item.name||'Voice note';let button=document.createElement('button');button.type='button';button.className='voice-play';button.setAttribute('aria-label','Play voice note');button.innerHTML=playIcon(false);let details=document.createElement('span');details.className='voice-details';let name=document.createElement('span');name.className='media-name';name.textContent=item.name||'Voice note';let time=document.createElement('span');time.className='voice-time';time.textContent='voice note';details.append(name,time);let progress=document.createElement('span');progress.className='voice-progress';let fill=document.createElement('span');progress.appendChild(fill);let open=document.createElement('a');open.className='media-open';open.href=url;open.target='_blank';open.rel='noopener';open.title='Open audio';open.setAttribute('aria-label','Open audio in viewer');open.innerHTML='<svg viewBox="0 0 24 24"><path d="M14 4h6v6M20 4l-9 9"/><path d="M18 13v6a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h6"/></svg>';let audio=new Audio(url);audio.preload='metadata';audio.onloadedmetadata=()=>time.textContent=clock(audio.duration);audio.ontimeupdate=()=>fill.style.width=(audio.duration?Math.min(100,audio.currentTime/audio.duration*100):0)+'%';audio.onplay=()=>{if(activeMediaAudio&&activeMediaAudio!==audio)activeMediaAudio.pause();activeMediaAudio=audio;button.innerHTML=playIcon(true);button.setAttribute('aria-label','Pause voice note')};audio.onpause=()=>{button.innerHTML=playIcon(false);button.setAttribute('aria-label','Play voice note')};audio.onended=()=>{fill.style.width='0';activeMediaAudio=null};button.onclick=()=>audio.paused?audio.play().catch(error=>status('Audio playback failed: '+error.message)):audio.pause();tile.append(button,details,open,progress);if(item.autoplay)setTimeout(()=>audio.play().catch(()=>{}),0);return tile}
function mediaTile(item){let kind=kindOf(item),url=item.url||objectUrl(item.file);if(kind==='audio'&&url)return audioTile(item,url);let tile=document.createElement(url?'a':'div');tile.className='media-tile '+kind;tile.title=item.name||kind;if(url){tile.href=url;tile.target='_blank';tile.rel='noopener'}let thumb=document.createElement('span');thumb.className='media-thumb';if(kind==='image'&&url){let image=document.createElement('img');image.src=url;image.alt='';thumb.appendChild(image)}else thumb.innerHTML=mediaIcon(kind);let name=document.createElement('span');name.className='media-name';name.textContent=item.name||kind;tile.append(thumb,name);return tile}
function mediaStrip(items){if(!items?.length)return null;let strip=document.createElement('div');strip.className='media-strip';items.forEach(item=>strip.appendChild(mediaTile(item)));return strip}
function voiceDatabase(){return new Promise((resolve,reject)=>{let request=indexedDB.open('mica-playground',1);request.onupgradeneeded=()=>request.result.createObjectStore('settings');request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error)})}
async function voiceSetting(mode,key,value){let db=await voiceDatabase();return new Promise((resolve,reject)=>{let transaction=db.transaction('settings',mode),store=transaction.objectStore('settings'),request=value===undefined?store.get(key):value===null?store.delete(key):store.put(value,key);request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error);transaction.oncomplete=()=>db.close()})}
async function persistTtsVoice(){if(!ttsVoiceBlob)return;await voiceSetting('readwrite','tts-reference',{blob:ttsVoiceBlob,name:ttsVoiceFilename||'Reference voice.wav',text:$('ttsVoiceText').value.trim()})}
function updateTtsVoiceUi(){let custom=$('ttsVoiceMode').value==='reference';$('ttsVoiceControls').classList.toggle('hidden',!custom);$('ttsVoiceName').textContent=ttsVoiceBlob?`${ttsVoiceFilename||'Reference voice'} · ${(ttsVoiceBlob.size/1024).toFixed(0)} KiB`:'No reference selected';$('ttsVoicePreview').classList.toggle('hidden',!ttsVoiceBlob);if(ttsVoiceBlob)$('ttsVoicePreview').src=objectUrl(ttsVoiceBlob);localStorage.micaTtsVoiceMode=$('ttsVoiceMode').value}
async function loadTtsVoice(){let saved=await voiceSetting('readonly','tts-reference');if(saved?.blob){ttsVoiceBlob=saved.blob;ttsVoiceFilename=saved.name||'Reference voice.wav';$('ttsVoiceText').value=saved.text||''}$('ttsVoiceMode').value=localStorage.micaTtsVoiceMode==='reference'?'reference':'default';updateTtsVoiceUi()}
async function normalizedVoiceFile(file){let decode=new AudioContext(),buffer=await decode.decodeAudioData(await file.arrayBuffer()),samples=new Float32Array(buffer.length);for(let channel=0;channel<buffer.numberOfChannels;channel++){let values=buffer.getChannelData(channel);for(let i=0;i<values.length;i++)samples[i]+=values[i]/buffer.numberOfChannels}await decode.close();if(buffer.duration>30)throw Error('Reference voice must be 30 seconds or shorter.');return encodeWav([samples],buffer.sampleRate)}
async function startTtsVoiceRecording(){if(recording)throw Error('Stop the message voice note before recording a TTS reference.');ttsVoiceStream=await navigator.mediaDevices.getUserMedia({audio:true});ttsVoiceCtx=new AudioContext();ttsVoiceChunks=[];let source=ttsVoiceCtx.createMediaStreamSource(ttsVoiceStream);ttsVoiceNode=ttsVoiceCtx.createScriptProcessor(4096,1,1);ttsVoiceNode.onaudioprocess=event=>ttsVoiceChunks.push(new Float32Array(event.inputBuffer.getChannelData(0)));source.connect(ttsVoiceNode);ttsVoiceNode.connect(ttsVoiceCtx.destination);ttsVoiceRecording=true;$('ttsVoiceRecord').textContent='Stop recording';$('ttsVoiceRecord').classList.add('recording');status('Recording the TTS reference voice…')}
async function stopTtsVoiceRecording(){ttsVoiceNode.disconnect();ttsVoiceStream.getTracks().forEach(track=>track.stop());ttsVoiceBlob=encodeWav(ttsVoiceChunks,ttsVoiceCtx.sampleRate);await ttsVoiceCtx.close();ttsVoiceRecording=false;ttsVoiceFilename='Recorded reference.wav';$('ttsVoiceRecord').textContent='Record sample';$('ttsVoiceRecord').classList.remove('recording');$('ttsVoiceMode').value='reference';await persistTtsVoice();updateTtsVoiceUi();status('TTS reference recorded. Add or verify its exact transcript.')}
const escapeHtml=value=>String(value??'').replace(/[&<>"']/g,character=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[character]));
function inlineMarkdown(source){let codes=[],value=escapeHtml(source);value=value.replace(/`([^`\n]+)`/g,(_,code)=>{let token=`MICAICODE${codes.length}TOKEN`;codes.push(`<code>${code}</code>`);return token});value=value.replace(/\[([^\]]+)\]\(((?:https?:\/\/|mailto:|\/)[^\s)]+)\)/gi,'<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');value=value.replace(/\*\*([^*\n]+)\*\*/g,'<strong>$1</strong>').replace(/__([^_\n]+)__/g,'<strong>$1</strong>').replace(/~~([^~\n]+)~~/g,'<del>$1</del>').replace(/(^|[\s(])\*([^*\n]+)\*(?=$|[\s).,!?:;])/g,'$1<em>$2</em>');codes.forEach((code,index)=>value=value.replace(`MICAICODE${index}TOKEN`,code));return value}
function setInline(element,text){element.innerHTML=inlineMarkdown(text)}
function markdownCells(line){let value=line.trim();if(value.startsWith('|'))value=value.slice(1);if(value.endsWith('|'))value=value.slice(0,-1);return value.split('|').map(cell=>cell.trim())}
function tableDivider(line){let cells=markdownCells(line);return cells.length>0&&cells.every(cell=>/^:?-{3,}:?$/.test(cell))}
function blockStart(lines,index){let line=lines[index]||'';return !line.trim()||/^\s*```/.test(line)||/^\s{0,3}#{1,4}\s+/.test(line)||/^\s*>\s?/.test(line)||/^\s*[-*+]\s+/.test(line)||/^\s*\d+[.)]\s+/.test(line)||/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line)||(index+1<lines.length&&line.includes('|')&&tableDivider(lines[index+1]))}
function renderMarkdown(target,source){target.innerHTML='';let lines=String(source||'').replace(/\r\n?/g,'\n').split('\n');for(let index=0;index<lines.length;){let line=lines[index];if(!line.trim()){index++;continue}let fence=line.match(/^\s*```\s*([\w+-]*)/);if(fence){let code=[],language=fence[1];index++;while(index<lines.length&&!/^\s*```/.test(lines[index]))code.push(lines[index++]);if(index<lines.length)index++;let pre=document.createElement('pre'),node=document.createElement('code');if(language)node.className='language-'+language;node.textContent=code.join('\n');pre.appendChild(node);target.appendChild(pre);continue}let heading=line.match(/^\s{0,3}(#{1,4})\s+(.+)$/);if(heading){let node=document.createElement('h'+heading[1].length);setInline(node,heading[2]);target.appendChild(node);index++;continue}if(/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line)){target.appendChild(document.createElement('hr'));index++;continue}if(index+1<lines.length&&line.includes('|')&&tableDivider(lines[index+1])){let table=document.createElement('table'),head=document.createElement('thead'),body=document.createElement('tbody'),headerRow=document.createElement('tr');markdownCells(line).forEach(cell=>{let th=document.createElement('th');setInline(th,cell);headerRow.appendChild(th)});head.appendChild(headerRow);table.append(head,body);index+=2;while(index<lines.length&&lines[index].includes('|')&&lines[index].trim()){let row=document.createElement('tr');markdownCells(lines[index++]).forEach(cell=>{let td=document.createElement('td');setInline(td,cell);row.appendChild(td)});body.appendChild(row)}target.appendChild(table);continue}let list=line.match(/^\s*([-*+]|\d+[.)])\s+(.+)$/);if(list){let ordered=/^\d/.test(list[1]),node=document.createElement(ordered?'ol':'ul');while(index<lines.length){let item=lines[index].match(/^\s*([-*+]|\d+[.)])\s+(.+)$/);if(!item||/^\d/.test(item[1])!==ordered)break;let li=document.createElement('li');setInline(li,item[2]);node.appendChild(li);index++}target.appendChild(node);continue}if(/^\s*>\s?/.test(line)){let quote=[],node=document.createElement('blockquote');while(index<lines.length&&/^\s*>\s?/.test(lines[index]))quote.push(lines[index++].replace(/^\s*>\s?/,''));setInline(node,quote.join('\n'));target.appendChild(node);continue}let paragraph=[];while(index<lines.length&&!blockStart(lines,index))paragraph.push(lines[index++]);if(!paragraph.length){paragraph.push(lines[index++])}let node=document.createElement('p');node.innerHTML=inlineMarkdown(paragraph.join('\n')).replace(/\n/g,'<br>');target.appendChild(node)}}
function verifyMarkdownRenderer(){let host=document.createElement('div');renderMarkdown(host,'## Heading\n\n- **bold** and `code`\n\n| A | B |\n| --- | --- |\n| one | two |\n\n<img src=x onerror=unsafe()>');if(!host.querySelector('h2')||!host.querySelector('ul strong')||!host.querySelector('code')||!host.querySelector('table')||host.querySelector('img')||!host.textContent.includes('<img src=x onerror=unsafe()>'))throw Error('Markdown renderer self-test failed.')}
function bubble(role,text,meta='',audio='',media=[]){if(audio)media=[...media,{name:'Voice reply.wav',kind:'audio',url:audio,autoplay:true}];let row=document.createElement('div');row.className='message-row '+role;let avatar=document.createElement('div');avatar.className='avatar '+(role==='user'?'user-avatar':'assistant-avatar');if(role==='user')avatar.innerHTML='<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="8" r="3.5"/><path d="M5.5 19c.8-3.4 3-5.2 6.5-5.2s5.7 1.8 6.5 5.2"/></svg>';else avatar.innerHTML='<img src="/mica-logo.svg" alt="">';let d=document.createElement('div');d.className='message'+(!text&&media.length?' media-only':'');let t=document.createElement('div');t.className='message-copy'+(role==='assistant'?' markdown':'');if(role==='assistant')renderMarkdown(t,text);else t.textContent=text;d.appendChild(t);let strip=mediaStrip(media);if(strip)d.appendChild(strip);let m=document.createElement('div');m.className='meta';m.textContent=meta;if(meta)d.appendChild(m);row.append(avatar,d);$('chat').appendChild(row);$('chat').scrollTop=$('chat').scrollHeight;return{row,box:d,text:t,meta:m}}
const stepLabels={receiving_input:'Receiving input',asr_transcribing:'Listening to your voice',temporary_files_ready:'Preparing attachments',agent_planning:'Planning the response',vlm_tool_running:'Reading attached media',llm_final:'Writing the answer',tts_generating:'Creating the voice reply',persisting_session:'Saving this turn'};
function showAgentStep(state){let activity=$('agentActivity');if(!state||state==='ready'){activity?.remove();return}if(!activity){activity=document.createElement('div');activity.id='agentActivity';activity.className='agent-activity';activity.setAttribute('role','status');activity.setAttribute('aria-live','polite');$('chat').appendChild(activity)}activity.textContent=stepLabels[state]||state.replaceAll('_',' ');$('chat').scrollTop=$('chat').scrollHeight}
function storedMedia(message,messageIndex){let items=Array.isArray(message.media)?message.media:[];if(!items.length){if(message.voice)items.push(message.voice);if(Array.isArray(message.attachments))items.push(...message.attachments);if(message.audio_path)items.push({name:'Voice reply.wav',kind:'audio',content_type:'audio/wav'});if(Array.isArray(message.audio_paths))items.push(...message.audio_paths.map((path,index)=>({name:`Voice reply ${index+1}.wav`,kind:'audio',content_type:'audio/wav'})))}return items.map((item,index)=>({...item,url:`/api/agent/session/${encodeURIComponent(sessionId)}/media/${messageIndex}/${index}`}))}
function renderSession(session){$('chat').innerHTML='';(session.messages||[]).forEach((message,index)=>{let text=message.role==='user'&&message.input_mode==='voice'?'':message.content;bubble(message.role,text||'','','',storedMedia(message,index))});$('sessionLabel').textContent=`session ${sessionId.slice(0,8)}`;renderHistory()}
async function loadSession(){status('Loading conversation…');let sr=await requireOk(await apiFetch('/api/agent/session/'+encodeURIComponent(sessionId))),sj=await sr.json();renderSession(sj);status('Ready.');return sj}
async function init(){verifyMarkdownRenderer();updateApiKeyUi();let r=await requireOk(await apiFetch('/api/models')),j=await r.json();rows=j.data||[];capability('llm','text','q4');capability('vlm','vision','q4');capability('asr','asr','q4');capability('tts','tts','q8');
 let tr=await requireOk(await apiFetch('/api/agent/tools')),tj=await tr.json(),desc=tj.data?.[0]?.function?.description||'';$('limits').textContent=desc.match(/Accepts at most[^.]+./)?.[0]||'';
 try{await loadTtsVoice()}catch{updateTtsVoiceUi()};ensureSession();await loadSession();status('Ready. Voice input automatically enables a spoken reply.')}
function encodeWav(parts,rate){let n=parts.reduce((a,x)=>a+x.length,0),b=new ArrayBuffer(44+n*2),v=new DataView(b),o=0;const str=s=>{for(let c of s)v.setUint8(o++,c.charCodeAt(0))};str('RIFF');v.setUint32(o,36+n*2,true);o+=4;str('WAVEfmt ');v.setUint32(o,16,true);o+=4;v.setUint16(o,1,true);o+=2;v.setUint16(o,1,true);o+=2;v.setUint32(o,rate,true);o+=4;v.setUint32(o,rate*2,true);o+=4;v.setUint16(o,2,true);o+=2;v.setUint16(o,16,true);o+=2;str('data');v.setUint32(o,n*2,true);o+=4;for(let p of parts)for(let x of p){x=Math.max(-1,Math.min(1,x));v.setInt16(o,x<0?x*32768:x*32767,true);o+=2}return new Blob([b],{type:'audio/wav'})}
function setRecordButton(active){let button=$('record');button.classList.toggle('recording',active);button.setAttribute('aria-label',active?'Stop recording':'Record voice');button.title=(active?'Stop recording':'Record voice')+(isMac?' (⌥R)':' (Alt+R)');button.innerHTML=active?'<svg viewBox="0 0 24 24"><rect x="7" y="7" width="10" height="10" rx="2"/></svg>':'<svg viewBox="0 0 24 24"><rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v3M9 21h6"/></svg>'}
async function startRecording(){stream=await navigator.mediaDevices.getUserMedia({audio:true});ctx=new AudioContext();chunks=[];let src=ctx.createMediaStreamSource(stream);node=ctx.createScriptProcessor(4096,1,1);node.onaudioprocess=e=>chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));src.connect(node);node.connect(ctx.destination);recording=true;setRecordButton(true);status('Recording voice note…')}
async function stopRecording(){node.disconnect();stream.getTracks().forEach(t=>t.stop());voiceBlob=encodeWav(chunks,ctx.sampleRate);await ctx.close();recording=false;setRecordButton(false);$('clearVoice').classList.remove('hidden');showFiles();status(`Voice note ready (${(voiceBlob.size/1024).toFixed(1)} KiB). It is the instruction when text is empty; otherwise it is context.`)}
$('record').onclick=()=>{(recording?stopRecording():startRecording()).catch(e=>status('ERROR: '+e))};$('clearVoice').onclick=()=>{voiceBlob=null;$('clearVoice').classList.add('hidden');showFiles();status('Voice note removed.')};
$('menuButton').onclick=()=>openSidebar(true);$('closeMenu').onclick=()=>openSidebar(false);$('scrim').onclick=()=>openSidebar(false);$('newChat').onclick=newSession;
$('settingsButton').onclick=()=>openSettings(!$('settingsPanel').classList.contains('open'));$('closeSettings').onclick=()=>openSettings(false);
$('saveApiKey').onclick=async()=>{apiKey=$('apiKey').value.trim();if(apiKey)sessionStorage.setItem('micaApiKey',apiKey);else sessionStorage.removeItem('micaApiKey');updateApiKeyUi();status(apiKey?'API key applied to this tab. Checking the server…':'API key cleared.');try{await init();openSettings(false)}catch(error){status('ERROR: '+error.message)}};
$('clearApiKey').onclick=()=>{apiKey='';sessionStorage.removeItem('micaApiKey');updateApiKeyUi();status('API key cleared. Authenticated requests now require a key.');$('apiKey').focus()};
$('apiKey').onkeydown=event=>{if(event.key==='Enter'){$('saveApiKey').click();event.preventDefault()}};
$('shortcutHelp').textContent=isMac?'⌘↵ send · ⌥R record':'Ctrl+Enter send · Alt+R record';$('send').title='Send message ('+(isMac?'⌘+Enter':'Ctrl+Enter')+')';setRecordButton(false);
function resizeComposer(){let input=$('text');input.style.height='auto';input.style.height=Math.min(input.scrollHeight,220)+'px'}
$('text').addEventListener('input',resizeComposer);$('text').addEventListener('keydown',event=>{if(event.key==='Enter'&&(event.metaKey||event.ctrlKey)){event.preventDefault();if(!$('send').disabled)$('send').click()}});
document.addEventListener('keydown',event=>{if(event.code==='KeyR'&&event.altKey&&!event.ctrlKey&&!event.metaKey&&!event.repeat){event.preventDefault();$('record').click()}if(event.key==='Escape'){openSidebar(false);openSettings(false)}});
document.addEventListener('click',event=>{if($('settingsPanel').classList.contains('open')&&!$('settingsPanel').contains(event.target)&&!$('settingsButton').contains(event.target))openSettings(false)});
$('exportSession').onclick=async()=>{try{status('Exporting conversation…');let response=await requireOk(await apiFetch('/api/agent/session/'+encodeURIComponent(sessionId)+'/export'));let blob=await response.blob(),link=document.createElement('a'),url=URL.createObjectURL(blob);link.href=url;link.download=`mica-session-${sessionId}.zip`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);status('Conversation archive exported.')}catch(error){status('ERROR: '+error.message)}};
$('importSession').onchange=async event=>{let archive=event.target.files[0];if(!archive)return;try{status('Importing conversation…');let form=new FormData();form.append('archive',archive,archive.name);let response=await requireOk(await apiFetch('/api/agent/session/import',{method:'POST',body:form})),result=await response.json();sessionId=result.session_id;localStorage.micaSessionId=sessionId;ensureSession('Imported conversation');await loadSession();openSidebar(false);status(`Imported ${result.media_files||0} media file(s).`)}catch(error){status('ERROR: '+error.message)}finally{event.target.value=''}};
$('ttsVoiceMode').onchange=()=>updateTtsVoiceUi();
$('ttsVoiceUpload').onchange=async event=>{let file=event.target.files[0];if(!file)return;try{status('Preparing reference voice…');ttsVoiceBlob=await normalizedVoiceFile(file);ttsVoiceFilename=file.name.replace(/\.[^.]+$/,'.wav');$('ttsVoiceMode').value='reference';await persistTtsVoice();updateTtsVoiceUi();status('TTS reference ready. Add or verify its exact transcript.')}catch(error){status('ERROR: '+error.message)}finally{event.target.value=''}};
$('ttsVoiceRecord').onclick=()=>{(ttsVoiceRecording?stopTtsVoiceRecording():startTtsVoiceRecording()).catch(error=>status('ERROR: '+error.message))};
$('ttsVoiceTranscribe').onclick=async()=>{if(!ttsVoiceBlob)return status('Record or upload a reference voice first.');try{status('Transcribing the reference voice…');let form=new FormData();form.append('model',model('asr'));form.append('file',ttsVoiceBlob,'tts-reference.wav');let response=await requireOk(await apiFetch('/api/transcribe',{method:'POST',body:form})),result=await response.json();$('ttsVoiceText').value=result.text||'';await persistTtsVoice();status('Reference transcript ready. Correct it if necessary.')}catch(error){status('ERROR: '+error.message)}};
$('ttsVoiceText').onchange=()=>persistTtsVoice().catch(error=>status('ERROR: '+error.message));
$('ttsVoiceRemove').onclick=async()=>{if(ttsVoiceRecording)await stopTtsVoiceRecording();ttsVoiceBlob=null;ttsVoiceFilename='';$('ttsVoiceText').value='';$('ttsVoicePreview').removeAttribute('src');$('ttsVoiceMode').value='default';await voiceSetting('readwrite','tts-reference',null);updateTtsVoiceUi();status('Using the built-in default TTS voice.')};
function currentInputMedia(){let items=selectedFiles.map(file=>({name:file.name,type:file.type,file}));if(voiceBlob)items.unshift({name:'Voice note.wav',type:'audio/wav',kind:'audio',file:voiceBlob});return items}
function showFiles(){let area=$('attachments');area.innerHTML='';let strip=mediaStrip(currentInputMedia());if(strip)area.appendChild(strip)}
$('files').onchange=e=>{selectedFiles=[...e.target.files];showFiles()};
const textExt=/\.(txt|md|json|csv|ya?ml|log|html|xml)$/i;
async function renderTextDocument(file){let text=await file.text(),canvas=document.createElement('canvas'),w=1240,h=1754,margin=70,lineH=26;canvas.width=w;canvas.height=h;let c=canvas.getContext('2d'),raw=text.replace(/\t/g,'    ').split(/\r?\n/),lines=[];c.font='20px ui-monospace, monospace';for(let line of raw){if(!line){lines.push('');continue}let part='';for(let word of line.split(/\s+/)){let next=part?part+' '+word:word;if(c.measureText(next).width>w-margin*2){lines.push(part);part=word}else part=next}lines.push(part)}let per=Math.floor((h-margin*2)/lineH),out=[];for(let p=0;p<Math.min(8,Math.ceil(lines.length/per));p++){c.fillStyle='white';c.fillRect(0,0,w,h);c.fillStyle='black';c.font='20px ui-monospace, monospace';lines.slice(p*per,(p+1)*per).forEach((line,i)=>c.fillText(line,margin,margin+(i+1)*lineH));let blob=await new Promise(ok=>canvas.toBlob(ok,'image/png'));out.push(new File([blob],`${file.name}-page-${p+1}.png`,{type:'image/png'}))}return out}
async function uploadFiles(){let out=[];for(let f of selectedFiles){if(textExt.test(f.name))out.push(...await renderTextDocument(f));else out.push(f)}return out}
let playbackCtx=null,playbackCursor=0;
async function playAudioChunk(base64){playbackCtx??=new AudioContext();let raw=atob(base64),bytes=new Uint8Array(raw.length);for(let i=0;i<raw.length;i++)bytes[i]=raw.charCodeAt(i);let decoded=await playbackCtx.decodeAudioData(bytes.buffer),source=playbackCtx.createBufferSource();source.buffer=decoded;source.connect(playbackCtx.destination);let start=Math.max(playbackCtx.currentTime+.03,playbackCursor);source.start(start);playbackCursor=start+decoded.duration}
function resetInput(){$('text').value='';$('text').style.height='auto';selectedFiles=[];voiceBlob=null;$('files').value='';$('clearVoice').classList.add('hidden');showFiles()}
async function appendFinalAssistantMedia(view){let response=await requireOk(await apiFetch('/api/agent/session/'+encodeURIComponent(sessionId))),session=await response.json();let messages=session.messages||[],index=messages.length-1,message=messages[index];if(!message||message.role!=='assistant')return;let items=storedMedia(message,index);if(!items.length)return;view.box.querySelectorAll(':scope > .media-strip').forEach(node=>node.remove());let strip=mediaStrip(items);if(strip)view.box.appendChild(strip)}
async function streamAgent(form){let r=await requireOk(await apiFetch('/api/agent/chat/stream',{method:'POST',body:form}));let view=bubble('assistant',''),reader=r.body.getReader(),decoder=new TextDecoder(),pending='',answer='',done=null;playbackCursor=0;while(true){let part=await reader.read();if(part.done)break;pending+=decoder.decode(part.value,{stream:true});for(;;){let end=pending.indexOf('\n\n');if(end<0)break;let block=pending.slice(0,end).replace(/\r/g,''),event='message',data='';pending=pending.slice(end+2);for(let line of block.split('\n')){if(line.startsWith('event:'))event=line.slice(6).trim();else if(line.startsWith('data:'))data+=line.slice(5).trim()}if(!data)continue;let value=JSON.parse(data);if(event==='state'){$('agentState').textContent=value.state.replaceAll('_',' ');showAgentStep(value.state)}else if(event==='transcript_delta')status('ASR: '+(status.transcript=(status.transcript||'')+value.delta));else if(event==='tool_call')status(`Using ${value.name}…`);else if(event==='tool_result')status('Media extracted; generating the answer…');else if(event==='text_delta'){answer+=value.delta;renderMarkdown(view.text,answer);$('chat').scrollTop=$('chat').scrollHeight}else if(event==='audio_chunk')await playAudioChunk(value.audio);else if(event==='done')done=value;else if(event==='error')throw Error(value.message||JSON.stringify(value))}}if(!done)throw Error('The stream ended without a done event.');await appendFinalAssistantMedia(view);showAgentStep(null);resetInput();status('Ready. The reply streamed live and its audio was saved as one final voice note.');return done}
$('send').onclick=async()=>{if(recording)await stopRecording();if(ttsVoiceRecording)await stopTtsVoiceRecording();let typed=$('text').value.trim();if(!typed&&!voiceBlob&&!selectedFiles.length){status('Add text, a voice note, or attachments first.');return}let useReference=voiceBlob&&$('ttsVoiceMode').value==='reference',referenceText=$('ttsVoiceText').value.trim();if(useReference&&(!ttsVoiceBlob||!referenceText)){status('Custom TTS voice requires a reference clip and its exact transcript.');openSettings(true);return}let title=typed||(voiceBlob?'Voice conversation':selectedFiles[0]?.name||'Media conversation');ensureSession(title);let outgoingMedia=currentInputMedia();bubble('user',typed,'','',outgoingMedia);$('send').disabled=true;$('agentState').textContent='working';showAgentStep('receiving_input');status.transcript='';try{if($('streamMode').checked&&voiceBlob){playbackCtx??=new AudioContext();await playbackCtx.resume()}let f=new FormData();f.append('session_id',sessionId);f.append('text',typed);f.append('llm_model',model('llm'));f.append('vlm_model',model('vlm'));f.append('asr_model',model('asr'));f.append('tts_model',model('tts'));if(voiceBlob)f.append('voice',voiceBlob,'voice.wav');if(useReference){f.append('tts_voice',ttsVoiceBlob,'tts-reference.wav');f.append('tts_voice_text',referenceText)}for(let file of await uploadFiles())f.append('files',file,file.name);status('Uploading → transcribing → agent tools → answering…');if($('streamMode').checked){await streamAgent(f)}else{let r=await requireOk(await apiFetch('/api/agent/chat',{method:'POST',body:f})),j=await r.json();bubble('assistant',j.answer,'',j.audio||'');resetInput();status('Ready. Non-stream mode waited for one final JSON response.')}}catch(e){bubble('assistant','ERROR: '+e.message);status('ERROR: '+e.message)}finally{showAgentStep(null);$('send').disabled=false;$('agentState').textContent='ready'}};
init().catch(e=>status('ERROR: '+e));
</script></html>"""


VOICE_HTML = r"""<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Mica voice pipeline</title>
<style>
body{font:16px system-ui;max-width:900px;margin:2rem auto;padding:0 1rem;background:#101418;color:#edf2f7}
fieldset{border:1px solid #3a4652;border-radius:12px;padding:1rem;margin:1rem 0}label{display:block;margin:.6rem 0}
select,textarea,input,button{font:inherit;padding:.65rem;border-radius:8px;border:1px solid #536170;background:#182029;color:#edf2f7}
textarea{width:calc(100% - 1.4rem);min-height:7rem}button{cursor:pointer;margin:.25rem}.primary{background:#2b6cb0}
.status{padding:.75rem;background:#17212b;border-radius:8px;white-space:pre-wrap}audio{width:100%;margin-top:1rem}a{color:#80bfff}
</style>
<h1>Mica voice pipeline</h1>
<p><a href="/multimodal">Open multimodal playground</a></p>
<fieldset><legend>Models</legend>
<label>ASR model <select id="asr"></select> Quantization <select id="asrQuant"></select></label>
<label>LLM model <select id="llm"></select> Quantization <select id="llmQuant"></select></label>
<label>TTS model <select id="tts"></select> Quantization <select id="ttsQuant"></select></label></fieldset>
<fieldset><legend>One-control pipeline</legend>
<button id="pipeline" class="primary">Start full pipeline</button>
<span>Click once to record; click the same button again to run ASR → LLM → TTS.</span></fieldset>
<fieldset><legend>1. Record</legend><button id="start" class="primary">Start microphone</button>
<button id="stop" disabled>Stop</button><span id="recordState"> idle</span></fieldset>
<fieldset><legend>2. Transcribe / edit</legend><button id="transcribe">Transcribe recording</button>
<textarea id="transcript" placeholder="Transcript appears here; you can also type text."></textarea></fieldset>
<fieldset><legend>3. Ask model and speak</legend><button id="chat">Send text to LLM</button><button id="speak">Speak response</button>
<textarea id="answer" placeholder="Model response"></textarea><audio id="audio" controls></audio></fieldset>
<div id="status" class="status">Loading models…</div>
<script>
const $=id=>document.getElementById(id); let ctx,stream,node,chunks=[],wavBlob,recording=false,pipelineArmed=false,modelRows=[];
const apiFetch=(url,options={})=>{let headers=new Headers(options.headers||{}),key=sessionStorage.getItem('micaApiKey')||'';if(key)headers.set('X-Mica-API-Key',key);return fetch(url,{...options,headers})};
const status=x=>$('status').textContent=x;
const baseId=x=>x.id.includes('@')?x.id.slice(0,x.id.lastIndexOf('@')):x.id;
function fillQuant(prefix,preferred){let base=$(prefix).value,q=$(prefix+'Quant'),rows=modelRows.filter(x=>baseId(x)===base);q.innerHTML='';
 rows.forEach(x=>q.add(new Option(`${x.quantization.toUpperCase()} · ${x.backend.toUpperCase()} · ${x.state}`,x.id)));
 let wanted=rows.findIndex(x=>x.quantization===preferred);if(wanted>=0)q.selectedIndex=wanted}
function fillCapability(prefix,cap,preferred){let s=$(prefix),bases=[...new Set(modelRows.filter(x=>x.capability===cap).map(baseId))];s.innerHTML='';
 bases.forEach(x=>s.add(new Option(x,x)));s.onchange=()=>fillQuant(prefix,preferred);fillQuant(prefix,preferred)}
const selectedModel=prefix=>$(prefix+'Quant').value;
async function models(){let r=await apiFetch('/api/models'),j=await r.json();if(!r.ok)throw Error(j.error?.message||'API key required. Open the main chat settings.');modelRows=j.data||[];
 fillCapability('asr','asr','q4');fillCapability('llm','text','q4');fillCapability('tts','tts','q8');
 status(`Ready. ${modelRows.length} model variants visible. TTS prefers Q8.`)}
function encodeWav(parts,rate){let n=parts.reduce((a,x)=>a+x.length,0),b=new ArrayBuffer(44+n*2),v=new DataView(b),o=0;
 const str=s=>{for(let c of s)v.setUint8(o++,c.charCodeAt(0))};str('RIFF');v.setUint32(o,36+n*2,true);o+=4;str('WAVEfmt ');
 v.setUint32(o,16,true);o+=4;v.setUint16(o,1,true);o+=2;v.setUint16(o,1,true);o+=2;v.setUint32(o,rate,true);o+=4;
 v.setUint32(o,rate*2,true);o+=4;v.setUint16(o,2,true);o+=2;v.setUint16(o,16,true);o+=2;str('data');v.setUint32(o,n*2,true);o+=4;
 for(let p of parts)for(let x of p){x=Math.max(-1,Math.min(1,x));v.setInt16(o,x<0?x*32768:x*32767,true);o+=2}return new Blob([b],{type:'audio/wav'})}
async function startRecording(){if(recording)return;stream=await navigator.mediaDevices.getUserMedia({audio:true});ctx=new AudioContext();chunks=[];wavBlob=null;
 let src=ctx.createMediaStreamSource(stream);node=ctx.createScriptProcessor(4096,1,1);node.onaudioprocess=e=>chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
 src.connect(node);node.connect(ctx.destination);recording=true;$('start').disabled=true;$('stop').disabled=false;$('recordState').textContent=' recording…';status('Recording microphone.')}
async function stopRecording(){if(!recording)throw Error('Microphone is not recording.');node.disconnect();stream.getTracks().forEach(t=>t.stop());wavBlob=encodeWav(chunks,ctx.sampleRate);await ctx.close();recording=false;
 $('start').disabled=false;$('stop').disabled=true;$('recordState').textContent=` captured ${(wavBlob.size/1024).toFixed(1)} KiB`;status('Recording ready for transcription.');return wavBlob}
$('start').onclick=()=>startRecording().catch(e=>status('ERROR: '+e));
$('stop').onclick=()=>stopRecording().then(()=>{pipelineArmed=false;$('pipeline').textContent='Start full pipeline'}).catch(e=>status('ERROR: '+e));
async function transcribe(){if(!wavBlob)throw Error('Record audio first.');let f=new FormData();f.append('model',selectedModel('asr'));f.append('file',wavBlob,'microphone.wav');
 status('Transcribing…');let r=await apiFetch('/api/transcribe',{method:'POST',body:f}),j=await r.json();if(!r.ok)throw Error(j.error?.message||JSON.stringify(j));$('transcript').value=j.text||'';return $('transcript').value}
async function chat(){let text=$('transcript').value.trim();if(!text)throw Error('Transcript/text is empty.');status('Generating response…');
 let r=await apiFetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({model:selectedModel('llm'),messages:[{role:'user',content:text}],temperature:0,max_tokens:512,stream:false})});
 let j=await r.json();if(!r.ok)throw Error(JSON.stringify(j));$('answer').value=j.choices?.[0]?.message?.content||'';return $('answer').value}
async function speak(){let text=$('answer').value.trim();if(!text)throw Error('Model response is empty.');status('Synthesizing speech…');
 let r=await apiFetch('/api/speech',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({model:selectedModel('tts'),input:text,response_format:'wav'})});
 if(!r.ok)throw Error(await r.text());let b=await r.blob();$('audio').src=URL.createObjectURL(b);await $('audio').play();status(`Complete. Generated ${(b.size/1024).toFixed(1)} KiB WAV.`)}
for(let [id,fn] of [['transcribe',transcribe],['chat',chat],['speak',speak]])$(id).onclick=()=>fn().catch(e=>status('ERROR: '+e));
$('pipeline').onclick=async()=>{if(!pipelineArmed){try{await startRecording();pipelineArmed=true;$('pipeline').textContent='Stop & run ASR → LLM → TTS';status('Full pipeline recording. Click the same button when you finish speaking.')}catch(e){status('ERROR: '+e)}return}
 $('pipeline').disabled=true;try{await stopRecording();status('Pipeline 1/3: transcribing…');await transcribe();status('Pipeline 2/3: generating response…');await chat();status('Pipeline 3/3: synthesizing speech…');await speak()}catch(e){status('ERROR: '+e)}finally{pipelineArmed=false;$('pipeline').disabled=false;$('pipeline').textContent='Start full pipeline'}};models().catch(e=>status('ERROR: '+e));
</script></html>"""


MULTIMODAL_HTML = r"""<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Mica multimodal playground</title>
<style>body{font:16px system-ui;max-width:900px;margin:2rem auto;padding:0 1rem;background:#101418;color:#edf2f7}
select,textarea,input,button{font:inherit;padding:.65rem;border-radius:8px;border:1px solid #536170;background:#182029;color:#edf2f7}
textarea{display:block;width:calc(100% - 1.4rem);min-height:8rem;margin:.8rem 0}button{cursor:pointer;background:#2b6cb0}pre{white-space:pre-wrap;background:#17212b;padding:1rem;border-radius:8px}a{color:#80bfff}</style>
<h1>Mica image/video playground</h1><p><a href="/voice">Open voice pipeline</a></p>
<label>Vision model <select id="model"></select> Quantization <select id="quant"></select></label><p><input id="file" type="file" accept="image/*,video/*"></p>
<textarea id="prompt">Describe the media accurately and preserve chronological order for video.</textarea>
<button id="send">Analyze media</button><pre id="output">Loading models…</pre>
<script>const $=x=>document.getElementById(x);let rows=[];const baseId=x=>x.id.includes('@')?x.id.slice(0,x.id.lastIndexOf('@')):x.id;
const apiFetch=(url,options={})=>{let headers=new Headers(options.headers||{}),key=sessionStorage.getItem('micaApiKey')||'';if(key)headers.set('X-Mica-API-Key',key);return fetch(url,{...options,headers})};
function quants(){let q=$('quant'),base=$('model').value,found=rows.filter(x=>baseId(x)===base);q.innerHTML='';found.forEach(x=>q.add(new Option(`${x.quantization.toUpperCase()} · ${x.backend.toUpperCase()} · ${x.state}`,x.id)));let preferred=found.findIndex(x=>x.quantization==='q4');if(preferred>=0)q.selectedIndex=preferred}
async function init(){let r=await apiFetch('/api/models'),j=await r.json();if(!r.ok)throw Error(j.error?.message||'API key required. Open the main chat settings.');rows=(j.data||[]).filter(x=>x.capability==='vision');
 [...new Set(rows.map(baseId))].forEach(x=>$('model').add(new Option(x,x)));$('model').onchange=quants;quants();$('output').textContent='Ready.'}
function uri(f){return new Promise((ok,no)=>{let r=new FileReader();r.onload=()=>ok(r.result);r.onerror=no;r.readAsDataURL(f)})}
$('send').onclick=async()=>{try{let f=$('file').files[0];if(!f)throw Error('Choose an image or video.');$('output').textContent='Running inference…';
 let u=await uri(f),video=f.type.startsWith('video/'),media=video?{type:'input_video',input_video:{data:u}}:{type:'image_url',image_url:{url:u}};
 let body={model:$('quant').value,messages:[{role:'user',content:[media,{type:'text',text:$('prompt').value}]}],temperature:0,max_tokens:1024,stream:false};
 let r=await apiFetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}),j=await r.json();
 if(!r.ok)throw Error(JSON.stringify(j));$('output').textContent=j.choices?.[0]?.message?.content||JSON.stringify(j,null,2)}catch(e){$('output').textContent='ERROR: '+e}};init().catch(e=>$('output').textContent='ERROR: '+e);
</script></html>"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    server_version = "MicaPlayground/1"

    def _send(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _api_key(self) -> str:
        return self.headers.get("X-Mica-API-Key", "").strip() or self.server.api_key

    def _authorization_headers(self, content_type: str | None = None) -> dict[str, str] | None:
        api_key = self._api_key()
        if not api_key:
            self._send(
                401,
                "application/json",
                json.dumps({
                    "error": {
                        "code": "api_key_required",
                        "message": "Mica API key required. Enter it in Settings.",
                    }
                }).encode(),
            )
            return None
        headers = {"Authorization": f"Bearer {api_key}"}
        if content_type:
            headers["Content-Type"] = content_type
        return headers

    def do_GET(self) -> None:  # noqa: N802
        if self.path in ("/", "/chat"):
            return self._send(200, "text/html; charset=utf-8", CHAT_HTML.encode())
        if self.path == "/mica-logo.svg":
            return self._send(
                200, "image/svg+xml; charset=utf-8",
                (ASSET_DIR / "mica-logo.svg").read_bytes(),
            )
        if self.path == "/voice":
            return self._send(200, "text/html; charset=utf-8", VOICE_HTML.encode())
        if self.path == "/multimodal":
            return self._send(200, "text/html; charset=utf-8", MULTIMODAL_HTML.encode())
        if self.path == "/health":
            return self._send(200, "application/json", b'{"status":"ok"}')
        if self.path == "/api/models":
            return self._proxy("GET", "/v1/models", b"", None)
        if self.path == "/api/agent/tools":
            return self._proxy("GET", "/v1/agent/tools", b"", None)
        if self.path.startswith("/api/agent/session/"):
            session_id = self.path.removeprefix("/api/agent/session/")
            return self._proxy("GET", "/v1/agent/sessions/" + session_id, b"", None)
        self._send(404, "application/json", b'{"error":"not_found"}')

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/api/agent/chat/stream":
            length = int(self.headers.get("Content-Length", "0"))
            return self._proxy_stream(
                "/v1/agent/chat/stream", self.rfile.read(length),
                self.headers.get("Content-Type", "application/octet-stream"),
            )
        routes = {
            "/api/chat": "/v1/chat/completions",
            "/api/transcribe": "/v1/audio/transcriptions",
            "/api/speech": "/v1/audio/speech",
            "/api/agent/chat": "/v1/agent/chat",
            "/api/agent/session/import": "/v1/agent/sessions/import",
        }
        target = routes.get(self.path)
        if not target:
            return self._send(404, "application/json", b'{"error":"not_found"}')
        length = int(self.headers.get("Content-Length", "0"))
        self._proxy("POST", target, self.rfile.read(length),
                    self.headers.get("Content-Type", "application/octet-stream"))

    def _proxy(self, method: str, path: str, body: bytes,
               content_type: str | None) -> None:
        headers = self._authorization_headers(content_type)
        if headers is None:
            return
        request = urllib.request.Request(
            self.server.mica_url + path, data=body if method == "POST" else None,
            headers=headers, method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.server.timeout) as response:
                self._send(response.status,
                           response.headers.get("Content-Type", "application/octet-stream"),
                           response.read())
        except urllib.error.HTTPError as error:
            self._send(error.code,
                       error.headers.get("Content-Type", "application/json"), error.read())
        except Exception as error:  # local diagnostic surface
            self._send(502, "application/json",
                       json.dumps({"error": str(error)}).encode())

    def _proxy_stream(self, path: str, body: bytes, content_type: str) -> None:
        headers = self._authorization_headers(content_type)
        if headers is None:
            return
        request = urllib.request.Request(
            self.server.mica_url + path, data=body,
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.server.timeout) as response:
                self.send_response(response.status)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                while line := response.readline():
                    self.wfile.write(line)
                    self.wfile.flush()
        except urllib.error.HTTPError as error:
            self._send(error.code,
                       error.headers.get("Content-Type", "application/json"), error.read())
        except Exception as error:
            encoded = ("event: error\ndata: " +
                       json.dumps({"message": str(error)}) + "\n\n").encode()
            try:
                self.wfile.write(encoded)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

    def log_message(self, fmt: str, *args) -> None:
        print(f"{self.address_string()} - {fmt % args}")


def load_config(path: Path) -> dict:
    if not path.is_file():
        return {}
    document = json.loads(path.read_text())
    if not isinstance(document, dict):
        raise ValueError(f"config must contain a JSON object: {path}")
    return document


def configured_key(args: argparse.Namespace, config: dict) -> str:
    ui = config.get("ui", {})
    if not isinstance(ui, dict):
        raise ValueError("server config field 'ui' must be an object")
    if args.api_key is not None:
        return args.api_key.strip()
    if args.api_key_file is not None:
        configured_file = args.api_key_file
    else:
        direct = ui.get("api_key", config.get("api_key", ""))
        if direct:
            return str(direct).strip()
        configured_file = ui.get("api_key_file", config.get(
            "api_key_file", "~/.mica/secrets/api-key"))
    key_file = Path(str(configured_file)).expanduser()
    return key_file.read_text().strip() if key_file.is_file() else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path,
                        default=Path.home() / ".mica/config/server.json")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--mica-url")
    parser.add_argument("--api-key",
                        help="Fallback API key (prefer --api-key-file to avoid shell history)")
    parser.add_argument("--api-key-file", type=Path)
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()
    config = load_config(args.config.expanduser())
    ui = config.get("ui", {})
    if not isinstance(ui, dict):
        raise ValueError("server config field 'ui' must be an object")
    host = args.host or ui.get("host", "127.0.0.1")
    port = args.port or int(ui.get("port", 8090))
    if args.mica_url:
        mica_url = args.mica_url
    elif ui.get("mica_url"):
        mica_url = ui["mica_url"]
    elif config.get("host") or config.get("port"):
        target_host = config.get("host", "127.0.0.1")
        if target_host in ("0.0.0.0", "::"):
            target_host = "127.0.0.1"
        mica_url = f"http://{target_host}:{int(config.get('port', 8080))}"
    else:
        mica_url = "http://127.0.0.1:8080"
    server = ThreadingHTTPServer((host, port), Handler)
    server.mica_url = str(mica_url).rstrip("/")
    server.api_key = configured_key(args, config)
    server.timeout = args.timeout
    print(f"Voice:      http://{host}:{port}/voice")
    print(f"Multimodal: http://{host}:{port}/multimodal")
    print(f"Agent chat: http://{host}:{port}/chat")
    if not server.api_key:
        print("Authentication: API key required in Chat Settings")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

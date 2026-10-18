"""Real proxy/process scheduling with tiny mock workers; no model downloads."""
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

BINARY, CONFIG = map(lambda x: Path(x).resolve(), sys.argv[1:3])
sys.argv = sys.argv[:1]
TOKEN = "scheduler_test_token_not_a_real_key"

MOCK = '''#!/usr/bin/env python3
import argparse,json,os,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
p=argparse.ArgumentParser();p.add_argument('--port',type=int);args,_=p.parse_known_args()
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  self.send_response(200);self.end_headers();self.wfile.write(b'{"status":"ok"}')
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers.get('Content-Length','0'))))
  if self.path=='/tokenize':data={'tokens':[1,2,3]}
  elif self.path=='/apply-template':data={'prompt':'rendered prompt'}
  else:
   if 'workload' in body or self.headers.get('X-Mica-Workload'):
    self.send_response(400);self.end_headers();return
   text=str(body.get('messages',[{}])[-1].get('content',''))
   if body.get('stream'):
    self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
    self.wfile.write(b'data: {"choices":[{"delta":{"content":"start"}}]}\\n\\n');self.wfile.flush();time.sleep(1)
    self.wfile.write(b'data: [DONE]\\n\\n');return
   if text=='hold':time.sleep(1)
   data={'worker_pid':os.getpid(),'forwarded':body,'choices':[{'message':{'content':'MICA_OK_31415'}}]}
  encoded=json.dumps(data).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(encoded)));self.end_headers();self.wfile.write(encoded)
ThreadingHTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
'''


class SchedulerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mica-global-scheduler-")
        self.root = Path(self.temp.name)
        self.server = None

    def tearDown(self):
        if self.server:
            self.server.terminate()
            try:
                self.server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
        self.temp.cleanup()

    def profile(self, name, quant, priority=100, input_tokens=512):
        content = f'''schema: 4
id: {name}
description: Isolated scheduling test.
mode: interactive
memory: {{required_ram_gib: 4, required_vram_gib: 0, safety_reserve_gib: 0.25}}
models:
  - id: spark-x25-4b
    engine: llama-cpp
    artifact: {{id: {quant}}}
    context: {{max_input_tokens: {input_tokens}, max_output_tokens: 128, max_total_tokens: {input_tokens+128}}}
    batching: {{max_concurrent_requests: 1}}
    kv_cache: {{precision: q8}}
    placement: {{mode: fixed, device: cpu, gpu_layers: 0}}
    residency: on-demand
    priority: {priority}
    startup: false
    idle_seconds: 300
'''
        path = self.root / "config/profiles" / f"{name}.yaml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def start(self, strategy="balanced", ram=16, priority=20):
        self.profile("app-a", "q4", priority)
        self.profile("app-b", "q8", 110)
        self.profile("shared", "q4", 110)
        self.profile("different-context", "q4", 110, 1024)
        runtime = self.root / "runtimes/llama.cpp/build-mica/bin/llama-server"
        runtime.parent.mkdir(parents=True)
        runtime.write_text(MOCK)
        runtime.chmod(0o755)
        model = self.root / "models/gguf/spark-x25-4b"
        model.mkdir(parents=True)
        for quant, filename in (("q4", "Spark-X2.5-4B-Q4_K_M.gguf"), ("q8", "Spark-X2.5-4B-Q8_0.gguf")):
            (model / filename).write_bytes(b"mock weights, never inference")
            (model / f".mica-complete-{quant}").write_text("miguelamendez/mica-spark-x25-4b\n6c242d2945f11bb3587c437a1bf1088666ca6e2f\n")
        state = {"schema":6,"installed_backends":["gguf"],"quantizations":["q4"],"default_quantization":"q4",
                 "profile":"app-a","profile_schema":4,"profile_mode":"interactive","profile_required_ram_gib":4,
                 "profile_required_vram_gib":0,"profile_memory_safety_reserve_gib":0.25,"profile_maximum_resident_workers":0,
                 "max_ram_gib":ram,"max_vram_gib":0,"gguf_target":"cpu","audio_target":"cpu","vllm_device":"cpu",
                 "api_key_file":"/unused","downloads":{}}
        state["configured_models"] = {"spark-x25-4b": {
            "enabled": True, "variants": {"gguf": ["q4"]}, "engine": "llama-cpp", "execution": "",
            "residency": "on-demand", "priority": priority, "startup": False, "idle_seconds": 300,
            "max_input_tokens": 512, "max_output_tokens": 128, "max_total_tokens": 640,
            "max_concurrent_requests": 1, "kv_cache_precision": "q8", "placement_mode": "fixed",
            "device": "cpu", "gpu_layers": 0, "ram_reservation_gib": -1, "vram_reservation_gib": -1}}
        (self.root / "state").mkdir()
        (self.root / "state/runtime.json").write_text(json.dumps(state))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1",0));self.port=probe.getsockname()[1]
        self.log = self.root / "server.log"
        with self.log.open("wb") as output:
            self.server = subprocess.Popen([str(BINARY),"serve","--root",str(self.root),"--config-dir",str(CONFIG),
                "--host","127.0.0.1","--port",str(self.port),"--api-key",TOKEN,"--strategy",strategy,
                *getattr(self,"extra_server_args",[])],stdout=output,stderr=output)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            try:
                self.get("/ready",auth=False);return
            except (OSError,urllib.error.HTTPError):
                if self.server.poll() is not None:self.fail(self.log.read_text())
                time.sleep(.05)
        self.fail(self.log.read_text())

    def get(self,path,auth=True):
        headers={"Authorization":"Bearer "+TOKEN} if auth else {}
        with urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{self.port}"+path,headers=headers),timeout=15) as r:return json.load(r)

    def chat(self, workload, text="hello", stream=False, header=False, **extra):
        payload={"messages":[{"role":"user","content":text}],"max_tokens":16,"stream":stream}
        payload.update(extra)
        headers={"Authorization":"Bearer "+TOKEN,"Content-Type":"application/json"}
        if header:headers["X-Mica-Workload"]=workload
        else:payload["workload"]=workload
        request=urllib.request.Request(f"http://127.0.0.1:{self.port}/v1/chat/completions",json.dumps(payload).encode(),headers)
        r=urllib.request.urlopen(request,timeout=20)
        if stream:return r
        with r:return json.load(r)

    def workers(self):return self.get("/admin/models")["workers"]

    def admin_post(self, path, payload):
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}"+path,
            json.dumps(payload).encode(), {"Authorization":"Bearer "+TOKEN,"Content-Type":"application/json"})
        with urllib.request.urlopen(request, timeout=15) as response:return json.load(response)

    def test_high_reuse_and_request_selection_does_not_change_default(self):
        self.start("high")
        a=self.chat("app-a")
        self.chat("app-b",header=True)
        self.assertEqual(len(self.workers()),2)
        shared=self.chat("shared")
        self.assertEqual(a["worker_pid"],shared["worker_pid"])
        self.assertEqual(json.loads((self.root/"state/runtime.json").read_text())["profile"],"app-a")
        self.assertEqual(self.get("/v1/models?workload=app-b")["data"][0]["quantization"],"q8")
        changed=self.chat("different-context")
        self.assertNotEqual(changed["worker_pid"],a["worker_pid"])

    def test_balanced_reconciles_low_priority_between_workloads(self):
        self.start("balanced",priority=20)
        self.chat("app-a");self.chat("app-b")
        self.assertEqual(len(self.workers()),1)
        self.assertIn("q8",self.workers()[0]["id"])

    def test_discovery_limits_follow_requested_workload_without_loading(self):
        self.start("single")
        for workload, input_tokens in (("app-a", 512), ("different-context", 1024)):
            payload = self.get("/v1/models?workload=" + workload)
            self.assertEqual(payload["profile"], workload)
            model = payload["data"][0]
            self.assertEqual(model["workload"], workload)
            self.assertEqual(model["max_input_tokens"], input_tokens)
            self.assertEqual(model["max_output_tokens"], 128)
            self.assertEqual(model["max_total_tokens"], input_tokens + 128)
            self.assertEqual(model["context_window"], model["max_total_tokens"])
            self.assertEqual(model["max_tokens"], model["max_output_tokens"])
            self.assertEqual(model["kv_cache_precision"], "q8")
            self.assertEqual(model["max_concurrent_requests"], 1)
            self.assertEqual(model["thinking_modes"], ["none", "on"])
            self.assertTrue(model["thinking_budget_supported"])
        self.assertFalse(self.workers())
        self.assertEqual(self.get("/v1/models")["profile"], "app-a")

    def test_native_llama_thinking_controls_are_forwarded_and_validated(self):
        self.start("single")
        for mode, enabled in (("none", False), ("on", True)):
            forwarded = self.chat("app-a", reasoning_effort=mode,
                                  thinking_budget_tokens=8)["forwarded"]
            self.assertEqual(forwarded["chat_template_kwargs"]["enable_thinking"], enabled)
            self.assertEqual(forwarded["reasoning_budget_tokens"], 8)
            self.assertNotIn("thinking_budget_tokens", forwarded)
            if mode == "on": self.assertNotIn("reasoning_effort", forwarded)
        for extra in ({"reasoning_effort": "high"},
                      {"reasoning_effort": "on", "chat_template_kwargs": {"enable_thinking": False}},
                      {"reasoning_effort": "on", "chat_template_kwargs": []},
                      {"thinking_budget_tokens": -1},
                      {"thinking_budget_tokens": 1, "reasoning_budget_tokens": 1}):
            with self.assertRaises(urllib.error.HTTPError) as failure: self.chat("app-a", **extra)
            self.assertEqual(failure.exception.code, 400)

    def test_single_waits_for_stream_then_offloads(self):
        self.start("single")
        with self.chat("app-a",stream=True) as stream:
            self.assertIn(b"start",stream.readline())
            with concurrent.futures.ThreadPoolExecutor() as pool:
                next_request=pool.submit(self.chat,"app-b")
                deadline=time.monotonic()+1
                while time.monotonic()<deadline and self.get("/admin/models")["scheduler"]["queued_requests"]==0:time.sleep(.02)
                self.assertFalse(next_request.done())
                self.assertEqual(len(self.workers()),1)
                stream.read()
                next_request.result(timeout=10)
        self.assertEqual(len(self.workers()),1)
        self.assertIn("q8",self.workers()[0]["id"])

    def test_memory_pressure_swaps_using_estimates(self):
        self.start("high",ram=8)
        self.chat("app-a");self.chat("app-b")
        self.assertEqual(len(self.workers()),1)
        state=self.get("/admin/models")
        self.assertLessEqual(state["reserved_ram_gib"],8)

    def test_oversized_worker_is_rejected_before_loading(self):
        self.start("high",ram=5)
        with self.assertRaises(urllib.error.HTTPError) as failure:self.chat("app-b")
        self.assertEqual(failure.exception.code,503)
        self.assertIn("estimated_footprint",failure.exception.read().decode())
        self.assertFalse(self.workers())

    def test_new_workload_is_discovered_without_restart(self):
        self.start("high")
        self.profile("added-later","q4",120)
        self.chat("added-later")
        self.assertIsNone(self.server.poll())
        self.assertEqual(self.get("/v1/models?workload=added-later")["profile"],"added-later")

    def test_balanced_keeps_shared_high_priority_workers(self):
        self.start("balanced")
        first = self.chat("app-a")
        self.chat("shared")
        self.chat("app-b")
        self.assertEqual(len(self.workers()), 2)
        self.assertEqual(first["worker_pid"], self.chat("shared")["worker_pid"])

    def test_strategy_hot_change_to_single_reconciles_residency(self):
        self.start("high")
        first = self.chat("app-a")
        self.chat("app-b")
        result = self.admin_post("/admin/scheduler", {"strategy":"single"})
        self.assertFalse(result["restart_required"])
        self.assertEqual(self.chat("app-a")["worker_pid"], first["worker_pid"])
        self.assertEqual(len(self.workers()), 1)
        self.assertEqual(self.get("/admin/models")["scheduler"]["strategy"], "single")
        with self.assertRaises(urllib.error.HTTPError):self.admin_post("/admin/scheduler", {"strategy":"invalid"})

    def test_default_selection_does_not_interrupt_bound_stream(self):
        self.start("high")
        with self.chat("app-a", stream=True) as stream:
            self.assertIn(b"start", stream.readline())
            self.admin_post("/admin/profile/activate", {"profile":"app-b"})
            self.assertEqual(self.get("/admin/models")["profile"]["name"], "app-b")
            self.assertIn(b"[DONE]", stream.read())
        self.assertEqual(self.get("/v1/models?workload=app-a")["profile"], "app-a")
        self.chat("app-a")
        # The persisted snapshot is complete enough to restart without setup.
        self.server.terminate();self.server.wait(timeout=10)
        with self.log.open("ab") as output:
            self.server = subprocess.Popen([str(BINARY),"serve","--root",str(self.root),"--config-dir",str(CONFIG),
                "--host","127.0.0.1","--port",str(self.port),"--api-key",TOKEN],stdout=output,stderr=output)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                self.get("/ready",auth=False);break
            except OSError:time.sleep(.05)
        self.assertIsNone(self.server.poll(), self.log.read_text())
        self.assertEqual(self.get("/v1/models")["profile"],"app-b")

    def test_request_concurrency_waits_instead_of_failing(self):
        self.start("high")
        with self.chat("app-a",stream=True) as stream:
            self.assertIn(b"start",stream.readline())
            with concurrent.futures.ThreadPoolExecutor() as pool:
                pending=pool.submit(self.chat,"app-a")
                time.sleep(.15)
                self.assertFalse(pending.done())
                self.assertEqual(self.workers()[0]["in_flight"],1)
                stream.read();pending.result(timeout=10)

    def test_conflicting_selectors_are_rejected(self):
        self.start("high")
        request=urllib.request.Request(f"http://127.0.0.1:{self.port}/v1/chat/completions",
            json.dumps({"workload":"app-a","messages":[{"role":"user","content":"hello"}]}).encode(),
            {"Authorization":"Bearer "+TOKEN,"Content-Type":"application/json","X-Mica-Workload":"app-b"})
        with self.assertRaises(urllib.error.HTTPError) as failure:urllib.request.urlopen(request,timeout=5)
        self.assertEqual(failure.exception.code,400)
        self.assertFalse(self.workers())


if __name__=="__main__":unittest.main()

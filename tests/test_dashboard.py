"""Native dashboard/API/credential regressions. Mock inference, no downloads."""
import importlib.util
import json
import subprocess
import sys
import time
import unittest
import urllib.error
import urllib.request
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import tarfile
import io
from pathlib import Path

spec = importlib.util.spec_from_file_location("scheduler_fixture", Path(__file__).with_name("test_global_scheduler.py"))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

class DashboardTests(unittest.TestCase):
    setUp = base.SchedulerTests.setUp
    tearDown = base.SchedulerTests.tearDown
    profile = base.SchedulerTests.profile
    chat = base.SchedulerTests.chat
    workers = base.SchedulerTests.workers
    def start(self):
        self.extra_server_args = ["--dashboard"]
        base.SchedulerTests.start(self, "high")
        self.admin = (self.root/"secrets/admin-api-key").read_text().strip()
        self.assertEqual((self.root/"secrets/admin-api-key").stat().st_mode & 0o777, 0o600)
    def request(self,path,body=None,key=None,headers=None):
        h={"Authorization":"Bearer "+(key if key is not None else self.admin),**(headers or {})}
        if body is not None:h["Content-Type"]="application/json"
        req=urllib.request.Request(f"http://127.0.0.1:{self.port}"+path,
            data=json.dumps(body).encode() if body is not None else None,headers=h)
        return urllib.request.urlopen(req,timeout=40)
    def get(self,path,auth=True):
        key=getattr(self,"admin",base.TOKEN) if auth else ""
        with self.request(path,key=key) as response:return json.load(response)
    def post(self,path,body,key=None,headers=None):
        with self.request(path,body,key,headers) as response:return json.load(response)
    def rejected(self,path,body=None,key=None,status=400,headers=None):
        with self.assertRaises(urllib.error.HTTPError) as failure:self.request(path,body,key,headers)
        self.assertEqual(failure.exception.code,status);failure.exception.close()
    def job(self,body):
        j=self.post("/admin/dashboard/jobs",body)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            j=self.get("/admin/dashboard/jobs/"+j["id"])
            if j["state"] != "running":return j
            time.sleep(.05)
        self.fail("management job did not finish")
    def test_assets_and_admin_scope(self):
        self.start()
        with self.request("/dashboard",key="") as response:
            self.assertEqual(response.status,200)
            self.assertIn("frame-ancestors 'none'",response.headers["Content-Security-Policy"])
            self.assertIn(b"Connect to Mica",response.read())
        self.rejected("/admin/dashboard/inventory",key=base.TOKEN,status=401)
        self.rejected("/admin/models",key=base.TOKEN,status=401)
        self.rejected("/admin/scheduler",{"strategy":"single"},key=base.TOKEN,status=401)
        self.assertEqual(self.get("/v1/models")["profile"],"app-a")
        inventory=self.get("/admin/dashboard/inventory")
        self.assertEqual(len(inventory["sections"]),7)
        self.assertNotIn(self.admin,json.dumps(inventory))
        self.assertNotIn(base.TOKEN,json.dumps(inventory))
        self.chat("app-a")
    def test_private_app_proxy_html_assets_multipart_stream_and_revocation(self):
        self.start()
        installed=self.job({"action":"app.install","id":"mica-assistant","accept_code":True})
        self.assertEqual(installed["state"],"completed",installed)
        seen=[]
        class App(BaseHTTPRequestHandler):
            def log_message(self,*_):pass
            def do_GET(self):
                seen.append((self.path,self.headers.get("Authorization"),self.headers.get("Cookie")))
                data=b'<html><img src="/logo.svg"><script>fetch("/api/models")</script></html>' if self.path=="/chat" else b'asset'
                self.send_response(200);self.send_header("Content-Type","text/html" if self.path=="/chat" else "text/javascript");self.end_headers();self.wfile.write(data)
            def do_POST(self):
                body=self.rfile.read(int(self.headers.get("Content-Length","0")));seen.append((self.path,body,self.headers.get("Origin")))
                if self.path=="/api/stream":
                    self.send_response(200);self.send_header("Content-Type","text/event-stream");self.end_headers()
                    self.wfile.write(b'data: first\n\n');self.wfile.flush();time.sleep(.5);self.wfile.write(b'data: done\n\n')
                else:
                    self.send_response(200);self.send_header("Content-Type","application/json");self.end_headers();self.wfile.write(b'{"ok":true}')
        app=ThreadingHTTPServer(("127.0.0.1",0),App);thread=threading.Thread(target=app.serve_forever,daemon=True);thread.start()
        state=self.root/"state/apps/mica-assistant.json";state.parent.mkdir(parents=True,exist_ok=True)
        birth=subprocess.check_output(["ps","-p",str(os.getpid()),"-o","lstart="],text=True)
        state.write_text(json.dumps({"pid":os.getpid(),"birth":birth,"url":f"http://127.0.0.1:{app.server_port}/chat"}))
        try:
            self.rejected("/apps/mica-assistant/chat",status=401)
            with self.request("/admin/dashboard/apps/mica-assistant/open",{}) as response:
                cookie=response.headers["Set-Cookie"].split(';')[0];opened=json.load(response)
                self.assertIn("HttpOnly",response.headers["Set-Cookie"])
            self.assertEqual(opened["url"],"/apps/mica-assistant/chat")
            with self.request(opened["url"],key="",headers={"Cookie":cookie}) as response:
                html=response.read();self.assertIn(b'src="/apps/mica-assistant/logo.svg"',html);self.assertIn(b'window.fetch=',html)
            with self.request("/apps/mica-assistant/logo.svg",key="",headers={"Cookie":cookie}) as response:self.assertEqual(response.read(),b'asset')
            self.assertEqual(seen[0][1:],(None,None),"Admin credentials must never reach app code")
            self.rejected(opened["url"],key="",headers={"Cookie":cookie,"Origin":"https://evil.invalid"},status=403)
            boundary="test-upload";payload=f'--{boundary}\r\nContent-Disposition: form-data; name="text"\r\n\r\nhello\r\n--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="sample.wav"\r\nContent-Type: audio/wav\r\n\r\nRIFFdata\r\n--{boundary}--\r\n'.encode()
            request=urllib.request.Request(f"http://127.0.0.1:{self.port}/apps/mica-assistant/api/upload",data=payload,headers={"Cookie":cookie,"Content-Type":"multipart/form-data; boundary="+boundary})
            with urllib.request.urlopen(request,timeout=10) as response:self.assertTrue(json.load(response)["ok"])
            self.assertIn(b'RIFFdata',seen[-1][1]);self.assertIn(b'hello',seen[-1][1]);self.assertEqual(seen[-1][2],f"http://127.0.0.1:{app.server_port}")
            started=time.monotonic()
            with self.request("/apps/mica-assistant/api/stream",{},key="",headers={"Cookie":cookie}) as stream:
                self.assertEqual(stream.readline(),b'data: first\n');self.assertLess(time.monotonic()-started,.4);self.assertIn(b'data: done',stream.read())
            self.admin=self.post("/admin/keys/rotate",{"kind":"admin"})["api_key"]
            self.rejected(opened["url"],key="",headers={"Cookie":cookie},status=401)
        finally:app.shutdown();app.server_close();thread.join()

    def test_device_download_verified_install_without_host_install_or_secrets(self):
        self.start()
        with self.request("/admin/dashboard/apps/mica-assistant/download",{"mode":"remote","server_url":f"http://127.0.0.1:{self.port}","workload":"app-a"}) as response:
            self.assertEqual(response.headers["Content-Type"],"application/gzip")
            archive=response.read()
        self.assertFalse((self.root/"apps/mica-assistant").exists())
        folder=self.root/"download";folder.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive),mode="r:gz") as tar:
            tar.extractall(folder,filter="data")
        package=next(folder.iterdir());metadata=json.loads((package/"client.json").read_text())
        self.assertEqual(metadata["suggested_workload"],"app-a")
        self.assertNotIn(self.admin,json.dumps(metadata));self.assertNotIn(base.TOKEN,json.dumps(metadata))
        device=self.root/"other-device"
        for _ in range(2):
            result=subprocess.run([sys.executable,str(package/"client.py"),"install","--root",str(device)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((device/"models").exists());self.assertTrue((device/"clients/mica-assistant/client.py").is_file())
    def test_admin_rotation_revokes_old_key_without_breaking_inference_stream(self):
        self.start();old=self.admin
        with self.chat("app-a",stream=True) as stream:
            self.assertIn(b"start",stream.readline())
            response=self.request("/admin/keys/rotate",{"kind":"admin"})
            self.assertEqual(response.headers["Cache-Control"],"no-store")
            rotated=json.load(response);response.close()
            self.admin=rotated["api_key"]
            self.assertFalse(rotated["restart_required"])
            self.assertIn(b"[DONE]",stream.read())
        self.rejected("/admin/models",key=old,status=401)
        self.assertEqual(self.get("/admin/models")["scheduler"]["strategy"],"high")
        self.chat("app-a")
        self.assertEqual((self.root/"secrets/api-key").read_text().strip(),base.TOKEN)
        self.assertEqual((self.root/"secrets/admin-api-key").read_text().strip(),self.admin)
    def test_inference_rotation_and_cross_origin_rejection(self):
        self.start()
        self.rejected("/admin/keys/rotate",{"kind":"admin"},key=base.TOKEN,status=401)
        self.rejected("/admin/keys/rotate",{"kind":"admin"},headers={"Origin":"https://malicious.invalid"},status=403)
        changed=self.post("/admin/keys/rotate",{"kind":"inference"})["api_key"]
        self.rejected("/v1/models",key=base.TOKEN,status=401)
        with self.request("/v1/models",key=changed) as response:self.assertEqual(response.status,200)
        self.assertEqual(self.get("/admin/models")["profile"]["name"],"app-a")
        saved = json.loads((self.root/"config/server.json").read_text())
        self.assertNotIn("api_key",saved)
        self.server.terminate();self.server.wait(timeout=10)
        with self.log.open("ab") as output:
            self.server=subprocess.Popen([str(base.BINARY),"serve","--root",str(self.root),"--config-dir",str(base.CONFIG),
                "--host","127.0.0.1","--port",str(self.port),"--dashboard"],stdout=output,stderr=output)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                self.get("/ready",auth=False);break
            except OSError:time.sleep(.05)
        self.assertIsNone(self.server.poll(),self.log.read_text())
        self.rejected("/v1/models",key=base.TOKEN,status=401)
        with self.request("/v1/models",key=changed) as response:self.assertEqual(response.status,200)
    def test_pause_resume_keeps_dashboard_and_blocks_inference(self):
        self.start();self.chat("app-a")
        self.post("/admin/inference/pause",{})
        self.assertFalse(self.workers())
        self.assertEqual(self.get("/admin/dashboard/inventory")["server"]["inference_status"],"paused")
        self.rejected("/v1/chat/completions",{"workload":"app-a","messages":[]},key=base.TOKEN,status=503)
        self.post("/admin/inference/resume",{});self.chat("app-a")
    def test_native_job_clone_and_app_install_do_not_restart_or_download_models(self):
        self.start();pid=self.server.pid
        cloned=self.job({"action":"workload.clone","id":"app-a","new_id":"copy-workload"})
        self.assertEqual(cloned["state"],"completed",cloned)
        self.assertTrue((self.root/"config/profiles/copy-workload.yaml").is_file())
        doc=self.get("/admin/dashboard/workloads/copy-workload")
        self.assertIn("copy-workload",doc["yaml"])
        installed=self.job({"action":"app.install","id":"mica-dictation","accept_code":True})
        self.assertEqual(installed["state"],"completed",installed)
        self.assertTrue((self.root/"apps/mica-dictation/receipt.json").is_file())
        self.assertEqual(pid,self.server.pid)
        self.assertFalse((self.root/"models/gguf/granite-speech-5").exists())
        self.assertNotIn(self.admin,json.dumps(installed))
        self.assertNotIn(base.TOKEN,json.dumps(installed))
    def test_job_validation_and_live_settings(self):
        self.start()
        for body in ({"action":"shell","command":"echo unsafe"},
                     {"action":"app.start","id":"mica-dictation"},
                     {"action":"workload.clone","id":"app-a","new_id":"../bad"},
                     {"action":"settings.save","values":{"api_key":"secret"}}):
            self.rejected("/admin/dashboard/jobs",body)
        result=self.job({"action":"settings.save","values":{"scheduling_strategy":"single"}})
        self.assertEqual(result["state"],"completed",result)
        self.assertEqual(self.get("/admin/models")["scheduler"]["strategy"],"single")
        self.assertEqual(json.loads((self.root/"config/server.json").read_text())["scheduling_strategy"],"single")

if __name__=="__main__":unittest.main()

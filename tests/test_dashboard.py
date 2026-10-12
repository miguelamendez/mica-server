"""Native dashboard/API/credential regressions. Mock inference, no downloads."""
import importlib.util
import json
import subprocess
import sys
import time
import unittest
import urllib.error
import urllib.request
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

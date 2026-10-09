"""Browser-settings route and JavaScript regression tests; no model downloads."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import threading
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('playground', ROOT / 'apps/mica_playground.py')
playground = importlib.util.module_from_spec(spec)
spec.loader.exec_module(playground)


class ChatSettingsTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'), 'Node is an optional frontend test dependency')
    def test_workload_selector_and_speed_labels(self):
        subprocess.run(['node', str(ROOT / 'tests/test_chat_settings.js')], check=True)
        inline = re.findall(r'<script>(.*?)</script>', playground.CHAT_HTML, re.S)
        self.assertTrue(inline)
        for script in inline:
            subprocess.run(['node', '--check'], input=script, text=True, check=True)

    def test_controls_and_speed_in_both_reply_modes(self):
        page = playground.CHAT_HTML
        for control in ['workloadSelect', 'switchWorkload', 'activeWorkload', 'showGenerationSpeed']:
            self.assertIn(f'id="{control}"', page)
        self.assertIn('done.generation_metrics', page)
        self.assertIn('showGenerationMetrics(view,j.generation_metrics)', page)
        self.assertIn('showGenerationMetrics(view,message.generation_metrics)', page)
        self.assertIn('localStorage.micaShowGenerationSpeed', page)

    def test_bridge_protects_listing_and_swap_and_preserves_rejections(self):
        calls = []
        class Backend(BaseHTTPRequestHandler):
            def do_GET(self):
                self.reply({'data': [{'id': 'test-workload'}], 'active_workload':'test-workload'})
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                self.reply({'profile':data['profile']}, 409 if data['profile']=='reject' else 200)
            def reply(self, body, status=200):
                calls.append((self.path,self.headers.get('Authorization')))
                payload=json.dumps(body).encode()
                self.send_response(status);self.send_header('Content-Type','application/json')
                self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
            def log_message(self,*args): pass
        backend=ThreadingHTTPServer(('127.0.0.1',0),Backend)
        ui=ThreadingHTTPServer(('127.0.0.1',0),playground.Handler)
        ui.api_key='';ui.mica_url=f'http://127.0.0.1:{backend.server_port}';ui.timeout=5
        threads=[threading.Thread(target=server.serve_forever) for server in [backend,ui]]
        for thread in threads:thread.start()
        base=f'http://127.0.0.1:{ui.server_port}'
        def request(path,model=None,key=None):
            return urllib.request.urlopen(urllib.request.Request(base+path,
                data=None if model is None else json.dumps({'profile':model}).encode(),
                headers={'Content-Type':'application/json',**({'X-Mica-API-Key':key} if key else {})}),timeout=5)
        try:
            for path,profile in [('/api/workloads',None),('/api/workloads/activate','new')]:
                with self.assertRaises(urllib.error.HTTPError) as rejected:request(path,profile)
                self.assertEqual(rejected.exception.code,401)
            self.assertEqual(calls,[])
            with request('/api/workloads',key='test-key') as response:self.assertEqual(response.status,200)
            with request('/api/workloads/activate','new','test-key') as response:
                self.assertEqual(json.load(response)['profile'],'new')
            with self.assertRaises(urllib.error.HTTPError) as rejected:
                request('/api/workloads/activate','reject','test-key')
            self.assertEqual(rejected.exception.code,409)
            self.assertEqual(calls[0],('/v1/workloads','Bearer test-key'))
            self.assertEqual(calls[1],('/admin/profile/activate','Bearer test-key'))
            with request('/mica-chat-settings.js') as response:self.assertIn(b'createWorkloadPicker',response.read())
        finally:
            for server in [ui,backend]:server.shutdown();server.server_close()
            for thread in threads:thread.join()

if __name__=='__main__':unittest.main()

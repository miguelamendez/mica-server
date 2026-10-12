"""Standard-library loopback client for browser dictation; no global hooks."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import urllib.error
import urllib.request


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, body, content_type="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def proxy(self):
        routes = {"/api/models": ("GET", "/v1/models"),
                  "/api/transcribe": ("POST", "/v1/audio/transcriptions"),
                  "/api/cleanup": ("POST", "/v1/chat/completions")}
        route = routes.get(self.path)
        if route is None or route[0] != self.command:
            return self.reply(404, b'{"error":"unknown route"}')
        # Prevent websites from turning this loopback bridge into an API-key relay.
        expected_host = f"127.0.0.1:{self.server.server_port}"
        if self.headers.get("Host") != expected_host or self.headers.get("Origin", f"http://{expected_host}") != f"http://{expected_host}":
            return self.reply(403, b'{"error":"origin rejected"}')
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if size < 0 or size > 32 * 1024 * 1024:
                return self.reply(413, b'{"error":"audio exceeds 32 MiB"}')
            if self.command == "POST" and not size:
                return self.reply(400, b'{"error":"empty request"}')
            body = self.rfile.read(size) if size else None
            headers = {"Content-Type": self.headers.get("Content-Type", "application/json")}
            # Read key/config for each request, so rotation doesn't require a client restart.
            config = json.loads(self.server.config.read_text()) if self.server.config.is_file() else {}
            host = config.get("host", "127.0.0.1")
            if host in ("0.0.0.0", "::"):
                host = "127.0.0.1"
            key = config.get("api_key", "")
            key_path = Path(config.get("api_key_file", str(self.server.config.parent.parent / "secrets/api-key"))).expanduser()
            if not key and key_path.is_file():
                key = key_path.read_text().strip()
            if key:
                headers["Authorization"] = f"Bearer {key}"
            url = f"http://{host}:{int(config.get('port', 8080))}{route[1]}"
            request = urllib.request.Request(url, body, headers, method=self.command)
            with urllib.request.urlopen(request, timeout=300) as response:
                self.reply(response.status, response.read(), response.headers.get("Content-Type", "application/json"))
        except urllib.error.HTTPError as error:
            self.reply(error.code, error.read())
        except Exception:
            self.reply(502, b'{"error":"Mica unavailable; check workload and configuration"}')

    def do_GET(self):
        if self.path == "/":
            return self.reply(200, Path(__file__).with_name("index.html").read_bytes(), "text/html; charset=utf-8")
        self.proxy()

    def do_POST(self):
        self.proxy()

    def log_message(self, *_):
        pass  # Never log dictated text, request bodies, credentials, or file names.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--port", type=int, default=8091)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.config = args.config
    print(f"Dictation: http://127.0.0.1:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

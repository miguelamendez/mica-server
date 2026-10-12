"""MCP stdio adapter for demo clients. No SDK, pip, listener, or local Mica required."""
import argparse
import json
import mimetypes
from pathlib import Path
import sys
import urllib.request
from urllib.parse import urlsplit
import uuid

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None  # Never relay an inference key to a redirected origin.


class Client:
    def __init__(self, app, config, root, connection="", workload="", allowed_directories=()):
        self.app, self.config, self.root = app, config, root
        self.connection, self.workload = connection, workload
        self.allowed_directories = [p.expanduser().resolve() for p in allowed_directories]

    def connection_info(self):
        path = self.root / "config/app-connections" / (self.app + ".json")
        connections = json.loads(path.read_text()) if path.is_file() else {}
        name = self.connection or connections.get("default", "")
        if name:
            return connections["connections"][name]
        config = json.loads(self.config.read_text()) if self.config.is_file() else {}
        host = config.get("host", "127.0.0.1")
        if host in ("0.0.0.0", "::"):
            host = "127.0.0.1"
        return {"url": config.get("ui", {}).get("mica_url", f"http://{host}:{config.get('port', 8080)}"),
                "api_key_file": config.get("api_key_file", str(self.root / "secrets/api-key")),
                "workload": self.workload or config.get("default_workload", "")}

    def request(self, route, body=None, fields=None, files=None):
        connection = self.connection_info()
        url = urlsplit(connection["url"])
        if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.path not in ("", "/") or url.query or url.fragment:
            raise ValueError("Invalid Mica origin")
        key = Path(connection["api_key_file"]).expanduser().read_text().strip()
        headers = {"Authorization": "Bearer " + key, "X-Mica-Workload": self.workload or connection.get("workload", "")}
        data = None
        if fields is not None:
            boundary = "mica-" + uuid.uuid4().hex
            chunks = []
            for name, value in fields.items():
                chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
            total = 0
            for name, raw in files or []:
                path = Path(raw).expanduser()
                if not any(path.resolve().is_relative_to(base) for base in self.allowed_directories):
                    raise ValueError("File uploads require an explicitly allowed directory")
                if not path.is_file() or path.stat().st_size > 32 * 1024 * 1024:
                    raise ValueError("Each upload must be a file of at most 32 MiB")
                payload = path.read_bytes()
                total += len(payload)
                if total > 64 * 1024 * 1024:
                    raise ValueError("Combined uploads exceed 64 MiB")
                filename = path.name.replace('"', '_').replace('\r', '_').replace('\n', '_')
                mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
                chunks.extend([f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{filename}"\r\nContent-Type: {mime}\r\n\r\n'.encode(), payload, b'\r\n'])
            chunks.append(f"--{boundary}--\r\n".encode())
            data = b"".join(chunks)
            headers["Content-Type"] = "multipart/form-data; boundary=" + boundary
        elif body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(connection["url"].rstrip("/") + route, data, headers)
        with urllib.request.build_opener(NoRedirect).open(request, timeout=300) as response:
            return json.load(response)

    def tools(self):
        common = [{"name": "models", "description": "List models in the connected workload.", "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False}}]
        if self.app == "mica-dictation":
            common.extend([
                {"name": "transcribe", "description": "Transcribe an explicitly supplied local audio file; optionally clean up using the workload LLM. Does not start recording or type into apps.", "inputSchema": {"type": "object", "properties": {"audio_path": {"type": "string"}, "cleanup": {"type": "boolean", "default": False}}, "required": ["audio_path"], "additionalProperties": False}},
                {"name": "cleanup", "description": "Correct punctuation/transcription mistakes while preserving meaning.", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"], "additionalProperties": False}},
            ])
        else:
            common.append({"name": "assistant_chat", "description": "Send text and optional explicitly supplied image/video/document files to the assistant workload. Reuse session_id for conversation continuity.", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "session_id": {"type": "string"}, "files": {"type": "array", "maxItems": 16, "items": {"type": "string"}}}, "required": ["text"], "additionalProperties": False}})
        return common

    def cleanup(self, text):
        response = self.request("/v1/chat/completions", {"stream": False, "max_tokens": 2048, "messages": [
            {"role": "system", "content": "Fix punctuation and obvious dictation errors only. Preserve meaning and language. Return only corrected text."}, {"role": "user", "content": text}]})
        return response["choices"][0]["message"]["content"]

    def call(self, name, arguments):
        if not isinstance(arguments, dict):
            raise ValueError("arguments must be an object")
        definition = next((x for x in self.tools() if x["name"] == name), None)
        if definition is None:
            raise ValueError("Unknown tool")
        schema = definition["inputSchema"]
        if set(arguments) - set(schema["properties"]) or set(schema.get("required", [])) - set(arguments):
            raise ValueError("Invalid tool arguments")
        for key, value in arguments.items():
            kind = schema["properties"][key]["type"]
            if kind == "string" and not isinstance(value, str) or kind == "boolean" and not isinstance(value, bool) or kind == "array" and (not isinstance(value, list) or len(value) > 16 or any(not isinstance(x, str) for x in value)):
                raise ValueError("Invalid argument type")
        if name == "models":
            return self.request("/v1/models")
        if name == "cleanup":
            return {"text": self.cleanup(arguments["text"])}
        if name == "transcribe":
            result = self.request("/v1/audio/transcriptions", fields={"response_format": "json"}, files=[("file", arguments["audio_path"])])
            if arguments.get("cleanup"):
                result["text"] = self.cleanup(result["text"])
            return result
        fields = {"text": arguments["text"]}
        if arguments.get("session_id"):
            fields["session_id"] = arguments["session_id"]
        return self.request("/v1/agent/chat", fields=fields, files=[("files", path) for path in arguments.get("files", [])])


def serve(client):
    for line in sys.stdin.buffer:
        if len(line) > 1024 * 1024:
            print(json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Message exceeds 1 MiB"}}), flush=True)
            continue
        request = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict) or request.get("jsonrpc") != "2.0":
                raise ValueError("Invalid JSON-RPC request")
            method = request.get("method", "")
            if "id" not in request:
                continue
            metadata = request.get("params", {}).get("_meta", {})
            modern = "io.modelcontextprotocol/protocolVersion" in metadata
            if modern:
                version = metadata["io.modelcontextprotocol/protocolVersion"]
                if version != "2026-07-28":
                    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32022, "message": "Unsupported protocol version", "data": {"supported": ["2026-07-28", "2025-11-25"], "requested": version}}}), flush=True)
                    continue
                if not isinstance(metadata.get("io.modelcontextprotocol/clientCapabilities"), dict):
                    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32602, "message": "Required client capabilities missing"}}), flush=True)
                    continue
            if method == "server/discover":
                if not modern:
                    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32602, "message": "Provide per-request MCP metadata; legacy clients may initialize instead"}}), flush=True)
                    continue
                result = {"supportedVersions": ["2026-07-28", "2025-11-25"], "capabilities": {"tools": {}}, "_meta": {"io.modelcontextprotocol/serverInfo": {"name": client.app, "version": "0.2.0"}}}
            elif method == "initialize":
                requested = request.get("params", {}).get("protocolVersion", "2024-11-05")
                version = requested if requested in ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25") else "2025-11-25"
                result = {"protocolVersion": version, "capabilities": {"tools": {}}, "serverInfo": {"name": client.app, "version": "0.2.0"}}
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": client.tools()}
            elif method == "tools/call":
                params = request.get("params", {})
                try:
                    output = client.call(params.get("name", ""), params.get("arguments", {}))
                    result = {"content": [{"type": "text", "text": json.dumps(output, ensure_ascii=False)}], "isError": False}
                except Exception:
                    # Never return credentials, local private paths, or server error bodies.
                    result = {"content": [{"type": "text", "text": "Tool failed. Check server, workload, inference key, file permissions and arguments."}], "isError": True}
            else:
                print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32601, "message": "Method not found"}}), flush=True)
                continue
            if modern:
                result["resultType"] = "complete"
                result.setdefault("_meta", {})["io.modelcontextprotocol/serverInfo"] = {"name": client.app, "version": "0.2.0"}
            print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
        except Exception:
            print(json.dumps({"jsonrpc": "2.0", "id": request.get("id") if isinstance(request, dict) else None, "error": {"code": -32700, "message": "Invalid request"}}), flush=True)


def main(app):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.home() / ".mica")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--connection", default="")
    parser.add_argument("--workload", default="")
    parser.add_argument("--allow-directory", type=Path, action="append", default=[], help="Allow explicit file uploads from this directory (repeatable); none by default")
    args = parser.parse_args()
    serve(Client(app, args.config or args.root / "config/server.json", args.root, args.connection, args.workload, args.allow_directory))

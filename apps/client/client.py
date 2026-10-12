"""Standalone demo client installer/connector manager; Python standard library only.

Run `python3 client.py install`, then `python3 client.py connect` and `start`.
Remote inference never installs Mica, engines, or weights on this device.
"""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit
import urllib.request
import webbrowser
import re

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, mode="w", delete=False) as stream:
        json.dump(data, stream, indent=2)
        temporary = Path(stream.name)
    temporary.chmod(0o600)
    temporary.replace(path)


def valid_id(value):
    return bool(value) and len(value) <= 80 and value[0].isalnum() and all(c.isascii() and (c.isalnum() or c in "_-") for c in value)


def valid_url(value):
    url = urlsplit(value)
    if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.query or url.fragment or url.path not in ("", "/"):
        raise ValueError("Use a credential-free HTTP(S) Mica origin, with no path/query.")
    _ = url.port
    return value.rstrip("/")


def read(path):
    return json.loads(path.read_text()) if path.is_file() else {}


def guarded_build(directory, command, ceiling):
    gib = 1024 ** 3
    if sys.platform == "darwin":
        pressure = int(subprocess.check_output(["sysctl", "-n", "kern.memorystatus_vm_pressure_level"], text=True))
        if pressure >= 2:
            raise ValueError("macOS memory pressure is elevated; free memory before building")
        total = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True)) / gib
        stats = subprocess.check_output(["vm_stat"], text=True)
        page_size = int(re.search(r"page size of (\d+) bytes", stats).group(1))
        pages = {name: int(count) for name, count in re.findall(r"(Pages [^:]+):\s+(\d+)", stats)}
        available = (pages.get("Pages free", 0) + max(pages.get("Pages purgeable", 0), pages.get("Pages inactive", 0) * .5)) * page_size / gib
    elif sys.platform.startswith("linux"):
        stats = {name: int(value) / 1048576 for name, value in re.findall(r"^(MemTotal|MemAvailable):\s+(\d+)", Path("/proc/meminfo").read_text(), re.M)}
        total, available = stats["MemTotal"], stats["MemAvailable"]
        maximum, current = Path("/sys/fs/cgroup/memory.max"), Path("/sys/fs/cgroup/memory.current")
        if maximum.is_file() and current.is_file() and maximum.read_text().strip() != "max":
            limit = int(maximum.read_text()) / gib
            total, available = min(total, limit), min(available, max(0, limit - int(current.read_text()) / gib))
    else:
        raise ValueError("Native client builds currently support macOS and Linux/WSL")
    budget = min(ceiling, total * .5, available - max(2, total * .2) - .5)
    if budget < 1:
        raise ValueError("Not enough safe build headroom. Free memory and retry; no build was started.")
    guard = [sys.executable, str(directory / "build_guard.py"), "--limit-gib", str(round(budget, 3))]
    if sys.platform == "darwin":
        guard += ["--macos-pressure-limit", "2"]
    subprocess.run(guard + ["--", *command], check=True)


def install(package, directory):
    for item in package["files"]:
        relative = Path(item["path"])
        if relative.is_absolute() or any(p in (".", "..") for p in relative.parts):
            raise ValueError("Unsafe package path")
        file = Path(__file__).parent / relative
        if file.is_symlink() or file.stat().st_size != item["size_bytes"] or hashlib.sha256(file.read_bytes()).hexdigest() != item["sha256"]:
            raise ValueError("Package checksum mismatch: " + str(relative))
    if directory.exists():
        print("Already installed. Use connect to add this server; existing files/connections were kept.")
        return
    directory.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".install-", dir=directory.parent))
    try:
        for item in package["files"]:
            target = stage / item["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(Path(__file__).parent / item["path"], target)
            target.chmod(0o700 if item.get("executable") else 0o600)
        shutil.copyfile(__file__, stage / "client.py")
        save(stage / "client.json", package)
        stage.rename(directory)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print("Installed client at", directory, "— no inference engines or models installed.")


def connect(package, root, name, url, workload):
    if not valid_id(name):
        raise ValueError("Invalid connection name")
    url = valid_url(url or package.get("suggested_server", "") or input("Mica server URL: "))
    workload = workload or package.get("suggested_workload", "") or input("Workload ID: ")
    key = getpass.getpass("Inference API key (not dashboard admin key): ").strip()
    if not key:
        raise ValueError("Inference key required")
    request = urllib.request.Request(url + "/v1/models?" + __import__("urllib.parse", fromlist=["urlencode"]).urlencode({"workload": workload}), headers={"Authorization": "Bearer " + key})
    with urllib.request.build_opener(NoRedirect).open(request, timeout=20) as response:
        models = json.load(response)
    operations = set()
    for model in models.get("data", []):
        declared = {x["operation"] for x in model.get("supported_interactions", [])}
        implemented = {x["operation"] for x in model.get("endpoint_contracts", [])}
        operations.update(declared & implemented)
    missing = set(package["requirements"]["operations"]) - operations
    if missing:
        raise ValueError("Remote workload lacks operations: " + ", ".join(sorted(missing)))
    key_file = root / "secrets" / "app-connections" / package["id"] / name
    key_file.parent.mkdir(parents=True, exist_ok=True)
    if key_file.is_symlink():
        raise ValueError("Key file cannot be a symlink")
    with tempfile.NamedTemporaryFile(dir=key_file.parent, mode="w", delete=False) as stream:
        stream.write(key + "\n")
        temporary = Path(stream.name)
    temporary.chmod(0o600)
    temporary.replace(key_file)
    path = root / "config/app-connections" / (package["id"] + ".json")
    connections = read(path) or {"schema": 1, "connections": {}}
    connections["connections"][name] = {"url": url, "api_key_file": str(key_file), "workload": workload}
    connections["default"] = name
    save(path, connections)
    print("Connected:", name, url, workload, "— key stored privately; no local server needed.")


def setup_local(package, root, directory):
    binary = shutil.which("mica-server") or str(Path.home() / ".local/bin/mica-server")
    if not Path(binary).is_file():
        print("Local inference requires Mica. This installs the C++/Lua server from its official source;")
        print("CMake, Git, a C++20 compiler and Lua development headers must already be installed.")
        if input("Build and install local Mica now? [y/N] ").lower() != "y":
            return
        checkout = Path(tempfile.mkdtemp(prefix="mica-client-build-"))
        try:
            subprocess.run(["git", "clone", "--depth", "1", "https://github.com/miguelamendez/mica-server.git", str(checkout / "source")], check=True)
            source = checkout / "source"
            guarded_build(directory, ["cmake", "-S", str(source), "-B", str(checkout / "build"), "-DCMAKE_BUILD_TYPE=Release"], 8)
            # Bundled existing guard uses live memory and macOS pressure; no unbounded build.
            guarded_build(directory, ["cmake", "--build", str(checkout / "build"), "--parallel", "1"], 8)
            subprocess.run(["cmake", "--install", str(checkout / "build"), "--prefix", str(Path.home() / ".local")], check=True)
        finally:
            shutil.rmtree(checkout)
    print("Choose a bundled workload for your hardware:")
    for item in package["workloads"]:
        print(item["id"], "—", item["description"])
    workload = input("Workload suffix (mac/cuda): ").strip()
    selected = next((w for w in package["workloads"] if w["id"] == workload), None)
    if selected is None:
        raise ValueError("Unknown workload")
    print("Preparing local engines/models may download many GB and consume memory.")
    if input("Prepare and start local inference? [y/N] ").lower() != "y":
        return
    # Native installation registers workloads and performs hardware-aware setup.
    inventory = json.loads(subprocess.check_output([binary, "app", "list", "--root", str(root)], text=True))
    if not any(item["id"] == package["id"] and item.get("installed") for item in inventory["apps"]):
        subprocess.run([binary, "app", "install", package["id"], "--accept-code", "--root", str(root)], check=True)
    registered = package["id"] + "--" + workload
    subprocess.run([binary, "app", "setup", package["id"], registered, "--root", str(root)], check=True)
    subprocess.run([binary, "start", "--workload", registered, "--root", str(root)], check=True)
    config = read(root / "config/server.json")
    host = config.get("host", "127.0.0.1")
    if host in ("0.0.0.0", "::"):
        host = "127.0.0.1"
    key_path = config.get("api_key_file", str(root / "secrets/api-key"))
    path = root / "config/app-connections" / (package["id"] + ".json")
    saved = read(path) or {"schema": 1, "connections": {}}
    saved["connections"]["local"] = {"url": f"http://{host}:{config.get('port', 8080)}", "api_key_file": key_path, "workload": registered}
    saved["default"] = "local"
    save(path, saved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "connect", "connections", "start", "setup-local", "mcp"])
    parser.add_argument("--root", type=Path, default=Path.home() / ".mica")
    parser.add_argument("--connection", default="")
    parser.add_argument("--url", default="")
    parser.add_argument("--workload", default="")
    parser.add_argument("--tui", action="store_true", help="Open the native Dictation setup TUI")
    parser.add_argument("--allow-directory", type=Path, action="append", default=[])
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    package = read(Path(__file__).with_name("client.json"))
    if not valid_id(package.get("id", "")):
        raise ValueError("Invalid client package")
    directory = root / "clients" / package["id"]
    if args.action == "install":
        install(package, directory)
        print("Next: python3", directory / "client.py", "connect (remote) or setup-local (local inference)")
        return
    if not directory.is_dir():
        raise ValueError("Install the client first")
    if args.action == "connect":
        return connect(package, root, args.connection or "remote", args.url, args.workload)
    if args.action == "setup-local":
        return setup_local(package, root, directory)
    if args.action == "mcp":
        if not (directory / "mcp.py").is_file():
            raise ValueError("This app does not provide an MCP entry point")
        command = [sys.executable, str(directory / "mcp.py"), "--root", str(root)]
        if args.connection:
            command += ["--connection", args.connection]
        for path in args.allow_directory:
            command += ["--allow-directory", str(path)]
        os.execv(sys.executable, command)
    connections = read(root / "config/app-connections" / (package["id"] + ".json"))
    if args.action == "connections":
        for name, data in connections.get("connections", {}).items():
            print(name, data["url"], data["workload"], "(default)" if name == connections.get("default") else "")
        return
    selected = args.connection or connections.get("default", "")
    connection = connections.get("connections", {}).get(selected)
    if not connection:
        raise ValueError("Add a server with connect first")
    config = {"ui": {"mica_url": connection["url"]}, "api_key_file": connection["api_key_file"]}
    config_path = root / "config/client-runtime" / (package["id"] + ".json")
    save(config_path, config)
    launch = package["launch"]
    replacements = {"{app_dir}": str(directory), "{mica_config}": str(config_path), "{port}": str(launch.get("port", 0)), "{workload}": connection["workload"]}
    command = [launch["executable"]]
    for raw in launch["arguments"]:
        for key, value in replacements.items():
            raw = raw.replace(key, value)
        command.append(raw)
    environment = os.environ.copy()
    if package["id"] == "mica-dictation":
        binary = shutil.which("mica-dictation")
        if not binary:
            binary = str(directory / "native/build/mica-dictation")
            if not Path(binary).is_file():
                print("Building the small native Dictation client only (not Mica Server). CMake and a C++ compiler are required.", flush=True)
                guarded_build(directory, ["cmake", "-S", str(directory / "native"), "-B", str(directory / "native/build"), "-DCMAKE_BUILD_TYPE=Release"], 2)
                guarded_build(directory, ["cmake", "--build", str(directory / "native/build"), "--parallel", "1"], 2)
        environment["MICA_DICTATION_BINARY"] = binary
        if args.tui:
            command.append("--tui")
    url = f"http://127.0.0.1:{launch['port']}{launch['path']}" if "port" in launch else "native Dictation (Ctrl+Alt/Option+R; --tui for setup)"
    print("App runs on this device:", url, "Inference:", connection["url"], flush=True)
    # The app stays in the foreground; Ctrl+C stops only this client.
    process = subprocess.Popen(command, cwd=directory, env=environment)
    try:
        import time
        time.sleep(.5)
        if process.poll() is not None:
            raise ValueError("App exited; check its output")
        if "port" in launch:
            webbrowser.open(url)
        process.wait()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print("Client:", str(error) or "stopped", file=sys.stderr)
        sys.exit(1)

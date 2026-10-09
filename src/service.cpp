#include "mica_server/service.hpp"
#include "mica_server/command.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/setup.hpp"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mica {
using json = nlohmann::json;
namespace {
json read_json(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  return json::parse(stream);
}
void private_write(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".pending";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("cannot write " + path.string());
  fchmod(fd, 0600);
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto n = write(fd, value.data() + offset, value.size() - offset);
    if (n <= 0) { close(fd); throw std::runtime_error("configuration write failed"); }
    offset += static_cast<std::size_t>(n);
  }
  close(fd);
  std::filesystem::rename(temporary, path);
}
std::string next_value(const std::vector<std::string>& args, std::size_t& i) {
  if (++i >= args.size()) throw std::invalid_argument("missing option value");
  return args[i];
}
}

bool local_server_running(const std::filesystem::path& root) {
  const int fd = open((root / "run/server.lock").c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
  close(fd);
  return held;
}

json local_server_request(const std::filesystem::path& root,
                          const std::string& method, const std::string& path,
                          const json& body) {
  if (!local_server_running(root)) throw std::runtime_error("Mica is stopped; run mica-server start");
  const auto control = read_json(root / "run/server.json");
  std::string host = control.at("host").get<std::string>();
  if (host == "0.0.0.0") host = "127.0.0.1";
  if (host == "::") host = "::1";
  std::ifstream key_stream(control.at("api_key_file").get<std::string>());
  std::string key;
  std::getline(key_stream, key);
  while (!key.empty() && (key.back() == '\r' || key.back() == '\n')) key.pop_back();
  if (key.empty()) throw std::runtime_error("CLI control requires the server to use an API-key file");
  httplib::Client client(host, control.at("port").get<int>());
  client.set_connection_timeout(2, 0);
  client.set_read_timeout(method == "GET" ? 2 : 40, 0);
  const httplib::Headers headers{{"Authorization", "Bearer " + key}};
  auto response = method == "POST" ? client.Post(path, headers, body.dump(), "application/json")
                                    : client.Get(path, headers);
  if (!response) throw std::runtime_error("server connection failed; inspect logs/server.log");
  const auto result = json::parse(response->body);
  if (response->status >= 400) throw std::runtime_error(result.dump());
  return result;
}

json local_server_status(const std::filesystem::path& root) {
  if (!local_server_running(root)) return {{"status", "stopped"}};
  try {
    auto result = local_server_request(root, "GET", "/admin/models", json::object());
    const auto control = read_json(root / "run/server.json");
    result["status"] = result.value("ready", false) ? "ready" : "warming";
    result["host"] = control.at("host");
    result["port"] = control.at("port");
    result["pid"] = control.at("pid");
    return result;
  } catch (const std::exception& e) { return {{"status", "starting_or_unreachable"}, {"error", e.what()}}; }
}

int service_command(const std::string& action, const std::vector<std::string>& arguments,
                    const std::filesystem::path& executable,
                    const std::filesystem::path& default_root) {
  auto root = default_root;
  std::vector<std::string> serve_args{executable.string(), "serve"};
  std::vector<std::string> install_args{executable.string(), "workload", "install"};
  std::string workload;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto& option = arguments[i];
    if (option == "--root") {
      root = next_value(arguments, i);
      serve_args.insert(serve_args.end(), {option, root.string()});
      install_args.insert(install_args.end(), {option, root.string()});
    } else if (option == "--workload") workload = next_value(arguments, i);
    else if (action == "start" && (option == "--host" || option == "--port" || option == "--server-config")) {
      const auto value = next_value(arguments, i);
      serve_args.insert(serve_args.end(), {option, value});
    } else if (action == "start" && (option == "--config-dir" || option == "--api-key-file")) {
      const auto value = next_value(arguments, i);
      serve_args.insert(serve_args.end(), {option, value});
      install_args.insert(install_args.end(), {option, value});
    } else if (action == "start" && (option == "--ram-gib" || option == "--vram-gib" || option == "--machine-file")) {
      const auto value = next_value(arguments, i);
      install_args.insert(install_args.end(), {option, value});
    } else throw std::invalid_argument("unknown " + action + " option: " + option);
  }
  if (action == "status") {
    std::cout << local_server_status(root).dump(2) << '\n';
    return 0;
  }
  if (action == "stop") {
    if (!local_server_running(root)) { std::cout << "Mica is already stopped\n"; return 0; }
    local_server_request(root, "POST", "/admin/server/stop", json::object());
    for (int i = 0; i < 100 && local_server_running(root); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (local_server_running(root)) throw std::runtime_error("server is still draining; check mica-server status");
    std::cout << "Mica stopped\n";
    return 0;
  }
  if (local_server_running(root)) {
    if (!workload.empty()) throw std::runtime_error("Mica is running; use workload activate " + workload);
    std::cout << local_server_status(root).dump(2) << '\n';
    return 0;
  }
  if (workload.empty() && std::filesystem::exists(root / "config/server.json"))
    workload = read_json(root / "config/server.json").value("default_workload", "");
  if (workload.empty() && std::filesystem::exists(root / "state/runtime.json"))
    workload = read_json(root / "state/runtime.json").value("profile", "");
  if (!workload.empty()) {
    install_args.insert(install_args.begin() + 3, workload);
    const auto installed = run_command(install_args);
    if (installed.exit_code) return installed.exit_code;
  } else if (install_args.size() > 3) {
    // root/config-directory select the existing runtime; allocation changes need a workload.
    if (std::find(install_args.begin(), install_args.end(), "--ram-gib") != install_args.end() ||
        std::find(install_args.begin(), install_args.end(), "--vram-gib") != install_args.end())
      throw std::invalid_argument("allocation flags require --workload");
  }
  if (!std::filesystem::exists(root / "state/runtime.json"))
    throw std::runtime_error("no workload installed; run start --workload ID");
  std::filesystem::create_directories(root / "logs");
  const auto log = root / "logs/server.log";
  const pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("cannot launch server");
  if (pid == 0) {
    setsid();
    const int output = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    const int input = open("/dev/null", O_RDONLY);
    if (output < 0 || input < 0) _exit(126);
    dup2(input, STDIN_FILENO); dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO);
    close(input); close(output);
    std::vector<char*> argv;
    for (auto& value : serve_args) argv.push_back(value.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(126);
  }
  for (int i = 0; i < 150; ++i) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid)
      throw std::runtime_error("server failed to start; inspect " + log.string());
    const auto state = local_server_status(root);
    if (state.value("pid", 0) == pid) {
      std::cout << state.dump(2) << '\n';
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  throw std::runtime_error("startup has not completed; inspect " + log.string());
}

int configuration_command(const std::vector<std::string>& args,
                          const std::filesystem::path& default_root) {
  if (args.empty()) throw std::invalid_argument("config requires show or set");
  auto root = default_root;
  json changes = json::object();
  std::filesystem::path import_key;
  bool rotate = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& option = args[i];
    if (option == "--root") root = next_value(args, i);
    else if (args[0] == "set" && option == "--rotate-api-key") rotate = true;
    else if (args[0] == "set" && option == "--api-key-file") import_key = next_value(args, i);
    else if (args[0] == "set" && option == "--host") changes["host"] = next_value(args, i);
    else if (args[0] == "set" && option == "--default-workload") changes["default_workload"] = next_value(args, i);
    else if (args[0] == "set" && option == "--port") changes["port"] = std::stoi(next_value(args, i));
    else if (args[0] == "set" && (option == "--ram-gib" || option == "--vram-gib"))
      changes[option.substr(2)] = std::stod(next_value(args, i));
    else throw std::invalid_argument("unknown config option: " + option);
  }
  const auto server_path = root / "config/server.json";
  const auto machine_path = root / "config/machine.yaml";
  auto server = std::filesystem::exists(server_path) ? read_json(server_path) : json::object();
  auto machine = std::filesystem::exists(machine_path) ? read_profile_file(machine_path)
                                                       : json{{"schema", 1}, {"limits", json::object()}};
  if (args[0] == "show") {
    const bool key = server.contains("api_key") || std::filesystem::exists(root / "secrets/api-key");
    server.erase("api_key");
    server["api_key_configured"] = key;
    std::cout << json{{"server", server}, {"machine", machine}}.dump(2) << '\n';
    return 0;
  }
  if (args[0] != "set") throw std::invalid_argument("config requires show or set");
  if (local_server_running(root)) throw std::runtime_error("stop Mica before changing server settings");
  for (const auto* key : {"host", "port", "default_workload"})
    if (changes.contains(key)) server[key] = changes[key];
  if (server.contains("port") && (server["port"].get<int>() < 1 || server["port"].get<int>() > 65535))
    throw std::invalid_argument("port must be 1..65535");
  if (changes.contains("ram-gib")) machine["limits"]["inference"]["ram_gib"] = changes["ram-gib"];
  if (changes.contains("vram-gib")) {
    const auto hardware = detect_hardware();
    json devices = json::object();
    for (const auto& accelerator : hardware.accelerators) {
      if (!accelerator.unified_memory && accelerator.memory_gib > 0)
        devices[accelerator.runtime + ":" + accelerator.id] = changes["vram-gib"];
    }
    if (devices.empty()) throw std::invalid_argument("no dedicated GPU detected; unified memory uses --ram-gib");
    machine["limits"]["inference"]["dedicated_memory_gib"] = devices;
  }
  // Validate before persisting. Global limits may not exceed detected capacity.
  resolve_machine_policy(detect_hardware(), machine_policy_from_document(machine));
  if (rotate && !import_key.empty()) throw std::invalid_argument("choose import or rotation, not both");
  if (rotate || !import_key.empty()) {
    std::string key = rotate ? generate_api_key() : "";
    if (!rotate) {
      std::ifstream stream(import_key);
      if (!stream) throw std::runtime_error("cannot read API-key file");
      std::getline(stream, key);
      if (!key.empty() && key.back() == '\r') key.pop_back();
      if (key.size() < 16 || key.size() > 512 ||
          std::any_of(key.begin(), key.end(), [](unsigned char c) { return c < 32 || c > 126; }))
        throw std::invalid_argument("API key must contain 16..512 printable ASCII characters");
    }
    const auto key_file = root / "secrets/api-key";
    private_write(key_file, key + "\n");
    server.erase("api_key");
    server["api_key_file"] = key_file.string();
  }
  private_write(machine_path, profile_yaml(machine));
  private_write(server_path, server.dump(2) + "\n");
  std::cout << "Settings saved. Start with --workload ID to reconcile the allocation and dependencies.\n";
  return 0;
}
}

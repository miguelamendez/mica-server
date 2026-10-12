#include "mica_server/dashboard.hpp"
#include "mica_server/tui.hpp"
#include "mica_server/service.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/config.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/setup.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/command.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <tuple>
#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace mica {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
json read(const fs::path& path) { std::ifstream f(path); if (!f) throw std::runtime_error("file unavailable"); return json::parse(f); }
std::string contents(const fs::path& path) { std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("asset unavailable"); return {std::istreambuf_iterator<char>(f), {}}; }
bool id_ok(const std::string& id) {
  return !id.empty() && id.size() <= 128 && std::isalnum(static_cast<unsigned char>(id[0])) &&
    std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}
void validate_job(const json& request) {
  if (!request.is_object() || !request.contains("action") || !request["action"].is_string()) throw std::invalid_argument("action required");
  static const std::map<std::string, std::set<std::string>> fields{
    {"workload.prepare", {"id"}}, {"workload.save", {"id", "yaml"}}, {"workload.clone", {"id", "new_id"}},
    {"app.install", {"id", "accept_code"}}, {"app.start", {"id", "workload", "accept_code"}},
    {"app.stop", {"id"}}, {"app.uninstall", {"id"}}, {"app.source-add", {"id", "url"}}, {"app.source-remove", {"id"}},
    {"settings.save", {"values"}}, {"update.check", {"channel"}}, {"update.install", {"channel"}}};
  const auto action = request["action"].get<std::string>();
  const auto it = fields.find(action); if (it == fields.end()) throw std::invalid_argument("unknown dashboard action");
  for (const auto& [key, _] : request.items()) if (key != "action" && !it->second.contains(key)) throw std::invalid_argument("unexpected action field: " + key);
  if (it->second.contains("id")) {
    const auto id = request.at("id").get<std::string>();
    const auto colon = id.find(':');
    if (!id_ok(id) && !(action.starts_with("app.") && colon != std::string::npos && id_ok(id.substr(0, colon)) && id_ok(id.substr(colon + 1))))
      throw std::invalid_argument("invalid ID");
  }
  if (request.contains("workload") && !id_ok(request["workload"].get<std::string>())) throw std::invalid_argument("invalid workload");
  if (request.contains("new_id") && !id_ok(request["new_id"].get<std::string>())) throw std::invalid_argument("invalid new ID");
  if (action == "app.install" || action == "app.start") if (!request.value("accept_code", false)) throw std::invalid_argument("explicit trusted-code consent required");
  if (action == "workload.save" && (!request.at("yaml").is_string() || request["yaml"].get<std::string>().size() > 65536)) throw std::invalid_argument("workload YAML exceeds 64 KiB");
  if (action.starts_with("update.") && request.value("channel", "source") != "source" && request.value("channel", "source") != "release") throw std::invalid_argument("invalid update channel");
  if (action == "settings.save") {
    const auto& values = request.at("values");
    if (!values.is_object() || values.empty()) throw std::invalid_argument("settings values required");
    const std::set<std::string> allowed{"host", "port", "default_workload", "scheduling_strategy", "ram_gib", "vram_gib"};
    for (const auto& [key, value] : values.items()) {
      if (!allowed.contains(key)) throw std::invalid_argument("unsupported setting: " + key);
      if (key == "port" && (!value.is_number_integer() || value < 1 || value > 65535)) throw std::invalid_argument("port must be 1..65535");
      if ((key == "ram_gib" || key == "vram_gib") && (!value.is_number() || value < 0)) throw std::invalid_argument("memory must be nonnegative");
      if (key == "host" && (!value.is_string() || value.get<std::string>().empty())) throw std::invalid_argument("host required");
    }
  }
}
void prepare(const fs::path& root, const fs::path& config, const std::string& id, bool preserve_default = true) {
  const auto state_path = root / "state/runtime.json";
  auto previous = fs::exists(state_path) ? read(state_path) : json();
  SetupOptions options; options.root = root; options.config_directory = config; options.profile = id;
  const auto machine = load_machine_policy(root / "config/machine.yaml");
  if (machine.inference_ram_gib) options.max_ram_gib = *machine.inference_ram_gib;
  auto registry = load_registry(config); merge_custom_models(registry, root); merge_installed_profiles(registry, root);
  if (!previous.is_null()) {
    options.ignore_context_limit = previous.value("ignore_context_limit", false);
    options.allow_partial_workload = previous.value("allow_partial_workload", false);
  }
  const auto restore = [&](bool failed = false) {
    if (previous.is_null() || (!preserve_default && !failed)) return;
    if (fs::exists(state_path)) {
      const auto installed = read(state_path);
      for (auto backend : installed.value("installed_backends", json::array())) {
        auto& list = previous["installed_backends"];
        if (std::find(list.begin(), list.end(), backend) == list.end()) list.push_back(backend);
      }
      const auto downloads = installed.value("downloads", json::object());
      for (const auto& [key, value] : downloads.items()) previous["downloads"][key] = value;
    }
    write_private_file(state_path, previous.dump(2));
  };
  try {
    auto resolved = resolve_setup(registry, options); execute_setup(registry, resolved);
    download_workload_models(registry, root); restore();
  } catch (...) { restore(true); throw; }
}
std::vector<std::string> command_for(const json& request, const fs::path& executable, const fs::path& root, const fs::path& config) {
  const auto action = request.at("action").get<std::string>();
  std::vector<std::string> command{executable.string()};
  if (action.starts_with("app.")) {
    command.insert(command.end(), {"app", action.substr(4), request.at("id")});
    if (action == "app.source-add") command.push_back(request.at("url"));
    if (action == "app.install" || action == "app.start") command.push_back("--accept-code");
    if (request.contains("workload")) command.insert(command.end(), {"--workload", request["workload"]});
    command.insert(command.end(), {"--config-dir", config.string()});
  } else if (action.starts_with("update.")) {
    command.insert(command.end(), {"update", "--channel", request.value("channel", "source")});
    command.push_back(action == "update.check" ? "--check" : "--restart");
  } else if (action == "settings.save") {
    command.insert(command.end(), {"config", "set"}); bool restart = false;
    const std::map<std::string, std::string> flags{{"host", "--host"}, {"port", "--port"}, {"default_workload", "--default-workload"},
      {"scheduling_strategy", "--strategy"}, {"ram_gib", "--ram-gib"}, {"vram_gib", "--vram-gib"}};
    for (const auto& [key, value] : request.at("values").items()) {
      command.insert(command.end(), {flags.at(key), value.is_string() ? value.get<std::string>() : value.dump()});
      restart |= key != "default_workload" && key != "scheduling_strategy";
    }
    if (restart) command.push_back("--restart");
    command.insert(command.end(), {"--config-dir", config.string()});
  } else throw std::invalid_argument("action is not a CLI job");
  command.insert(command.end(), {"--root", root.string()}); return command;
}
struct Jobs {
  std::mutex mutex;
  ServerOptions options;
  DashboardHost host;
  std::string active;
  bool maintenance{false}, resume_after{false};
  std::atomic<bool> stopping{false};
  std::thread monitor;
  ~Jobs() { stopping.store(true); if (monitor.joinable()) monitor.join(); }
  fs::path base() const { return options.root / "state/dashboard-jobs"; }
  json get(const std::string& id) {
    if (!id_ok(id)) throw std::invalid_argument("invalid job ID");
    auto job = read(base() / (id + ".json"));
    const auto pid = job.value("pid", 0); if (pid > 0) waitpid(pid, nullptr, WNOHANG);
    if (active == id && job.value("state", "") != "running") {
      if (maintenance) {
        try { host.reload(); if (resume_after) host.resume(); }
        catch (const std::exception& error) { job["state"] = "failed"; job["error"] = error.what(); }
      }
      active.clear(); maintenance = false;
    }
    const auto logfile = base() / (id + ".log");
    if (fs::exists(logfile)) {
      std::ifstream log(logfile, std::ios::binary); log.seekg(0, std::ios::end);
      const auto size = static_cast<std::streamoff>(log.tellg()); log.seekg(std::max<std::streamoff>(0, size - 32768));
      job["log_tail"] = std::string(std::istreambuf_iterator<char>(log), {});
    }
    job.erase("request"); job.erase("root"); job.erase("config"); job.erase("executable"); return job;
  }
  json submit(const json& request) {
    validate_job(request); std::lock_guard lock(mutex);
    if (!active.empty()) { const auto previous_id = active; (void)get(previous_id); if (!active.empty()) throw std::runtime_error("another management job is running"); }
    const auto action = request.at("action").get<std::string>();
    maintenance = action == "workload.prepare" || action == "workload.save";
    resume_after = host.status().value("inference_status", "") != "paused";
    if (maintenance) host.pause();
    const auto id = generate_api_key().substr(0, 24);
    fs::create_directories(base());
    const auto job_path = base() / (id + ".json"), logfile = base() / (id + ".log");
    json job{{"id", id}, {"action", action}, {"state", "running"}, {"request", request},
      {"root", options.root.string()}, {"config", options.config_directory.string()}, {"executable", options.executable.string()}};
    write_private_file(job_path, job.dump());
    std::vector<std::string> args{options.executable.string(), "dashboard-job", "--root", options.root.string(), "--id", id};
    std::vector<char*> native; for (auto& arg : args) native.push_back(arg.data()); native.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = -1; const int result = posix_spawn(&pid, native[0], &actions, nullptr, native.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result) { if (maintenance && resume_after) host.resume(); maintenance = false; throw std::runtime_error("management worker launch failed"); }
    // Do not rewrite the worker's final status (it may already have finished).
    active = id;
    return {{"id", id}, {"action", action}, {"state", "running"}};
  }
};
}

int dashboard_job_command(const std::vector<std::string>& args) {
  fs::path root; std::string id;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (++i >= args.size()) throw std::invalid_argument("missing management worker option");
    if (args[i-1] == "--root") root = args[i]; else if (args[i-1] == "--id") id = args[i]; else throw std::invalid_argument("unknown worker option");
  }
  if (!root.is_absolute() || !id_ok(id)) throw std::invalid_argument("invalid management worker path");
  const auto path = root / "state/dashboard-jobs" / (id + ".json");
  if (fs::is_symlink(path)) throw std::invalid_argument("job cannot be a symlink");
  auto job = read(path); const fs::path config = job.at("config"), executable = job.at("executable");
  job["pid"] = getpid(); write_private_file(path, job.dump());
  int code = 0;
  try {
    const auto& request = job.at("request"); validate_job(request);
    const auto action = request.at("action").get<std::string>();
    if (action == "workload.prepare") prepare(root, config, request.at("id"));
    else if (action == "workload.clone" || action == "workload.save") {
      auto registry = load_registry(config); merge_custom_models(registry, root); merge_installed_profiles(registry, root);
      const auto target = action == "workload.clone" ? request.at("new_id").get<std::string>() : request.at("id").get<std::string>();
      const auto draft = path.string() + ".yaml";
      if (action == "workload.clone") {
        if (registry.profiles.contains(target)) throw std::invalid_argument("new workload ID already exists");
        auto doc = profile_to_document(registry.profile(request.at("id"))); doc["id"] = target; write_profile_file(draft, doc);
      } else write_private_file(draft, request.at("yaml"));
      auto isolated = registry; const auto doc = read_profile_file(draft);
      if (profile_from_document(isolated, doc).name != target) throw std::invalid_argument("YAML ID must match selected workload");
      const auto prior = registry.profiles.contains(target) ? profile_to_document(registry.profile(target)) : json();
      install_profile_file(registry, root, draft); fs::remove(draft);
      if (action == "workload.save") {
        const auto current = read(root / "state/runtime.json").at("profile").get<std::string>();
        try { prepare(root, config, current, false); }
        catch (...) {
          if (!prior.is_null()) write_profile_file(root / "config/profiles" / (target + ".yaml"), prior);
          throw;
        }
      }
    } else {
      const auto result = run_command(command_for(request, executable, root, config)); code = result.exit_code;
    }
    job["state"] = code ? "failed" : "completed"; job["exit_code"] = code;
  } catch (const std::exception& error) { code = 1; job["state"] = "failed"; job["error"] = error.what(); }
  write_private_file(path, job.dump()); return code;
}

void register_dashboard(httplib::Server& server, const ServerOptions& options,
                        std::shared_ptr<Credentials> credentials, DashboardHost host) {
  const auto assets = options.config_directory.parent_path() / "apps/dashboard";
  auto jobs = std::make_shared<Jobs>(); jobs->options = options; jobs->host = host;
  jobs->monitor = std::thread([ptr=jobs.get()] {
    while (!ptr->stopping.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      std::lock_guard lock(ptr->mutex);
      if (!ptr->active.empty()) { const auto id = ptr->active; try { (void)ptr->get(id); } catch (...) {} }
    }
  });
  auto auth = [credentials](const httplib::Request& request, httplib::Response& response) {
    if (!credentials->allows(request.get_header_value("Authorization"), true)) {
      response.status = 401; response.set_content("{\"error\":\"admin_key_required\"}", "application/json"); return false;
    }
    const auto origin = request.get_header_value("Origin"), authority = request.get_header_value("Host");
    if (!origin.empty() && origin != "http://" + authority && origin != "https://" + authority) {
      response.status = 403; response.set_content("{\"error\":\"origin_rejected\"}", "application/json"); return false;
    }
    response.set_header("Cache-Control", "no-store"); return true;
  };
  for (const auto& [route, file, mime] : std::vector<std::tuple<std::string,std::string,std::string>>{
      {"/dashboard", "index.html", "text/html"}, {"/dashboard/", "index.html", "text/html"},
      {"/dashboard/dashboard.css", "dashboard.css", "text/css"}, {"/dashboard/dashboard.js", "dashboard.js", "text/javascript"},
      {"/dashboard/logo.svg", "logo.svg", "image/svg+xml"}}) {
    if (!fs::is_regular_file(assets / file)) throw std::runtime_error("dashboard assets missing; install binary and packaged files together");
    server.Get(route, [path=assets/file, mime](const auto&, auto& response) {
      response.set_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
      response.set_header("X-Content-Type-Options", "nosniff"); response.set_header("Referrer-Policy", "no-referrer");
      response.set_content(contents(path), mime);
    });
  }
  const auto guarded = [auth](auto handler) { return [auth, handler](const auto& request, auto& response) {
    if (!auth(request, response)) return;
    try { response.set_content(handler(request).dump(), "application/json"); }
    catch (const std::exception& error) { response.status = 400; response.set_content(json{{"error",error.what()}}.dump(), "application/json"); }
  }; };
  server.Get("/admin/dashboard/inventory", guarded([options,host](const auto&) {
    auto inventory = tui_inventory(options.root, options.config_directory, host.status());
    inventory["endpoints"] = server_endpoints(true);
    inventory["machine_policy"] = fs::exists(options.root / "config/machine.yaml") ? read_profile_file(options.root / "config/machine.yaml") : json::object();
    return inventory;
  }));
  server.Post("/admin/inference/pause", guarded([host](const auto&) { host.pause(); return host.status(); }));
  server.Post("/admin/inference/resume", guarded([host,jobs](const auto&) {
    std::lock_guard lock(jobs->mutex); if (!jobs->active.empty()) throw std::runtime_error("wait for maintenance job to complete"); host.resume(); return host.status();
  }));
  server.Post("/admin/dashboard/jobs", guarded([jobs](const auto& request) {
    if (request.body.size() > 131072) throw std::invalid_argument("management request too large");
    return jobs->submit(json::parse(request.body));
  }));
  server.Get(R"(/admin/dashboard/jobs/([A-Za-z0-9_.-]+))", guarded([jobs](const auto& request) {
    std::lock_guard lock(jobs->mutex); return jobs->get(request.matches[1].str());
  }));
  server.Get(R"(/admin/dashboard/workloads/([A-Za-z0-9_.-]+))", guarded([options](const auto& request) {
    const auto id = request.matches[1].str(); if (!id_ok(id)) throw std::invalid_argument("invalid workload ID");
    auto registry = load_registry(options.config_directory); merge_custom_models(registry, options.root); merge_installed_profiles(registry, options.root);
    return json{{"id", id}, {"yaml", profile_yaml(profile_to_document(registry.profile(id)))}};
  }));
}
}

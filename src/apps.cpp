#include "mica_server/apps.hpp"
#include "mica_server/command.hpp"
#include "mica_server/download.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/config.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/inventory.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/service.hpp"
#include "mica_server/credentials.hpp"
#include "mica_server/setup.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace mica {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::uint64_t max_package = 256ULL * 1024 * 1024;
bool id_ok(const std::string& s) { return std::regex_match(s, std::regex("[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}")); }
bool hash_ok(const std::string& s) { return std::regex_match(s, std::regex("[0-9a-f]{64}")); }
void fields(const json& j, std::initializer_list<const char*> allowed) {
  if (!j.is_object()) throw std::invalid_argument("app field must be a mapping");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (std::none_of(allowed.begin(), allowed.end(), [&](auto key) { return it.key() == key; }))
      throw std::invalid_argument("unknown app field: " + it.key());
}
void path_ok(const std::string& name) {
  const fs::path path(name);
  if (name.empty() || name.size() > 512 || path.is_absolute() || !std::regex_match(name, std::regex("[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*")))
    throw std::invalid_argument("invalid relative app path");
  for (const auto& part : path) if (part == ".." || part == "." || part.empty()) throw std::invalid_argument("app path traversal");
}
void https(const std::string& url) {
  if (!std::regex_match(url, std::regex("https://[A-Za-z0-9][A-Za-z0-9.-]*(?::[0-9]+)?/[^\\s]*")) ||
      url.find('@') != std::string::npos || url.find('#') != std::string::npos)
    throw std::invalid_argument("app download/source must be credential-free HTTPS");
}
json read_json(const fs::path& p) { if (!fs::exists(p)) return json::object(); std::ifstream f(p); return json::parse(f); }
void save(const fs::path& path, const json& j) {
  fs::create_directories(path.parent_path()); const auto temporary = path.string() + ".tmp";
  std::ofstream out(temporary); out << j.dump(2) << '\n'; out.close();
  if (!out) throw std::runtime_error("cannot save app registry");
  chmod(temporary.c_str(), 0600); fs::rename(temporary, path);
}
struct Lock {
  int fd;
  explicit Lock(const fs::path& root) {
    fs::create_directories(root / "apps");
    fd = open((root / "apps/.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) { if (fd >= 0) close(fd); throw std::runtime_error("another app operation is active"); }
  }
  ~Lock() { close(fd); }
};
std::string digest(const fs::path& path) {
#ifdef __APPLE__
  auto r = run_command({"shasum", "-a", "256", path.string()}, true);
#else
  auto r = run_command({"sha256sum", path.string()}, true);
#endif
  if (r.exit_code || r.output.size() < 64 || !hash_ok(r.output.substr(0, 64))) throw std::runtime_error("cannot compute app SHA-256");
  return r.output.substr(0, 64);
}
std::string fetch_text(const std::string& url) {
  https(url);
  auto r = run_command({"curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https",
    "--proto-redir", "=https", "--max-time", "30", "--max-filesize", "2097152", url}, true);
  if (r.exit_code || r.output.size() > 2097152) throw std::runtime_error("cannot fetch app manifest/index (HTTPS, 2 MiB limit)");
  return r.output;
}
json yaml_text(const std::string& text, const fs::path& directory) {
  fs::create_directories(directory); const auto file = directory / "manifest.yaml";
  std::ofstream out(file); out << text; out.close();
  if (!out) throw std::runtime_error("cannot stage app manifest");
  return read_profile_file(file);
}
// Pin repository discovery to an immutable revision, never execute repository code.
std::string source_base(const std::string& input) {
  https(input); std::string url = input; while (url.back() == '/') url.pop_back();
  const std::regex github("https://github.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)");
  const std::regex hf("https://huggingface.co/((?:datasets/|spaces/)?[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)");
  std::smatch match;
  if (std::regex_match(url, match, github)) {
    auto repo = match[1].str(); if (repo.ends_with(".git")) repo.resize(repo.size() - 4);
    const auto r = run_command({"git", "ls-remote", "https://github.com/" + repo + ".git", "HEAD"}, true);
    const auto sha = r.output.substr(0, 40);
    if (r.exit_code || !std::regex_match(sha, std::regex("[0-9a-f]{40}"))) throw std::runtime_error("cannot pin GitHub repository HEAD");
    return "https://raw.githubusercontent.com/" + repo + "/" + sha + "/";
  }
  if (std::regex_match(url, match, hf)) {
    auto repo = match[1].str(); auto kind = std::string("models/");
    if (repo.starts_with("datasets/")) { kind = "datasets/"; repo.erase(0, 9); }
    if (repo.starts_with("spaces/")) { kind = "spaces/"; repo.erase(0, 7); }
    const auto metadata = json::parse(fetch_text("https://huggingface.co/api/" + kind + repo + "/revision/main"));
    const auto sha = metadata.at("sha").get<std::string>();
    if (!std::regex_match(sha, std::regex("[0-9a-f]{40}"))) throw std::runtime_error("cannot pin Hugging Face revision");
    return url + "/resolve/" + sha + "/";
  }
  return url; // Direct index/manifest HTTPS URL.
}
json discovery(const std::string& source, const fs::path& directory) {
  auto base = source_base(source); std::string text, fetched;
  if (base.ends_with('/')) {
    bool found = false;
    for (const auto& suffix : {"apps/apps.yaml", "apps.yaml", "app.yaml"}) {
      try { text = fetch_text(base + suffix); fetched = base + suffix; found = true; break; } catch (const std::exception&) {}
    }
    if (!found) throw std::runtime_error("repository needs apps/apps.yaml, apps.yaml, or app.yaml");
  } else { fetched = base; text = fetch_text(base); }
  const auto document = yaml_text(text, directory);
  json apps = json::array();
  if (document.contains("apps")) {
    validate_app_index(document);
    for (const auto& item : document.at("apps")) {
      const auto url = fetched.substr(0, fetched.rfind('/') + 1) + item.at("manifest").get<std::string>();
      apps.push_back({{"id", item.at("id")}, {"name", item.at("name")}, {"description", item.at("description")},
        {"manifest_url", url}, {"manifest_sha256", item.at("sha256")}});
    }
  } else {
    validate_app_manifest(document);
    std::ofstream out(directory / "downloaded.yaml"); out << text; out.close();
    apps.push_back({{"id", document.at("id")}, {"name", document.at("name")}, {"description", document.at("description")},
      {"manifest_url", fetched}, {"manifest_sha256", digest(directory / "downloaded.yaml")}});
  }
  return {{"source", source}, {"resolved_url", fetched}, {"apps", apps}};
}
fs::path bundled(const fs::path& config) { return config.parent_path() / "apps"; }
std::string birth(int pid) {
  if (pid < 2) return {};
  const auto result = run_command({"ps", "-p", std::to_string(pid), "-o", "lstart="}, true);
  return result.exit_code ? "" : result.output;
}
bool running(const json& state) {
  const int pid = state.value("pid", 0);
  return pid > 1 && !state.value("birth", "").empty() && birth(pid) == state.value("birth", "");
}
json find_app(const json& inventory, const std::string& id) {
  for (const auto& app : inventory.at("apps")) if (app.at("id") == id) return app;
  throw std::invalid_argument("unknown app: " + id);
}
void plain_file(const fs::path& base, const std::string& relative) {
  path_ok(relative); auto cursor = base;
  if (fs::is_symlink(base)) throw std::runtime_error("app directory cannot be a symlink");
  for (const auto& part : fs::path(relative)) { cursor /= part; if (fs::is_symlink(cursor)) throw std::runtime_error("app symlink rejected"); }
  if (!fs::is_regular_file(cursor)) throw std::runtime_error("app file missing: " + relative);
}
}

void validate_app_manifest(const json& j) {
  fields(j, {"schema", "id", "name", "description", "version", "source", "distribution", "requirements", "permissions", "launch", "workloads", "interfaces"});
  if(j.contains("interfaces")) {const std::set<std::string> allowed{"cli","mcp","tui","web","background"};if(!j["interfaces"].is_array())throw std::invalid_argument("interfaces must be an array");for(const auto& item:j["interfaces"])if(!allowed.contains(item.get<std::string>()))throw std::invalid_argument("unknown app interface");}
  if (j.at("schema") != 1 || !id_ok(j.at("id").get<std::string>())) throw std::invalid_argument("invalid app schema/id");
  for (auto key : {"name", "description", "version"}) if (j.at(key).get<std::string>().empty()) throw std::invalid_argument("app needs " + std::string(key));
  const auto& source = j.at("source"); fields(source, {"repository", "path", "revision"});
  https(source.at("repository")); if (source.at("path") != ".") path_ok(source.at("path"));
  if (source.contains("revision") && !std::regex_match(source.at("revision").get<std::string>(), std::regex("[0-9a-f]{40}([0-9a-f]{24})?"))) throw std::invalid_argument("source revision must be an immutable commit digest");
  const auto& dist = j.at("distribution"); fields(dist, {"format", "files"});
  if (dist.at("format") != "files" || !dist.at("files").is_array() || dist.at("files").empty() || dist.at("files").size() > 512)
    throw std::invalid_argument("app distribution requires 1–512 verified files");
  std::set<std::string> paths; std::uint64_t total = 0;
  for (const auto& file : dist.at("files")) {
    fields(file, {"path", "sha256", "size_bytes", "url", "executable"}); auto p = file.at("path").get<std::string>(); path_ok(p);
    if (p == "app.yaml" || p == "receipt.json" || p.starts_with(".mica") || !paths.insert(p).second || !hash_ok(file.at("sha256")))
      throw std::invalid_argument("invalid/duplicate/reserved app file or SHA-256");
    if (!file.at("size_bytes").is_number_unsigned() && !file.at("size_bytes").is_number_integer()) throw std::invalid_argument("invalid file size");
    auto size = file.at("size_bytes").get<std::int64_t>();
    if (size < 1 || size > 64 * 1024 * 1024 || (total += size) > max_package) throw std::invalid_argument("app file/package exceeds size limit");
    if (file.contains("url")) https(file.at("url"));
    if (file.contains("executable") && !file.at("executable").is_boolean()) throw std::invalid_argument("executable file flag must be boolean");
  }
  // A file may not also be a directory prefix of another file.
  for (const auto& p : paths) for (const auto& q : paths) if (q.starts_with(p + "/")) throw std::invalid_argument("app file/directory collision");
  const auto& requirements = j.at("requirements"); fields(requirements, {"operations"});
  const std::set<std::string> operations{"chat.generate", "text.generate", "audio.transcribe", "audio.translate", "audio.synthesize_speech", "audio.diarize", "embedding.generate", "image.generate", "image.edit", "video.generate", "video.edit", "audio.generate", "audio.transform", "decisions.score"};
  if (!requirements.at("operations").is_array()) throw std::invalid_argument("operations must be an array");
  for (const auto& op : requirements.at("operations")) if (!operations.contains(op.get<std::string>())) throw std::invalid_argument("unknown app operation");
  const std::set<std::string> permissions{"microphone", "clipboard", "files", "network", "accessibility", "global-hotkeys"};
  if (!j.at("permissions").is_array()) throw std::invalid_argument("permissions must be an array");
  for (const auto& p : j.at("permissions")) if (!permissions.contains(p.get<std::string>())) throw std::invalid_argument("unknown app permission");
  const auto& launch = j.at("launch"); fields(launch, {"executable", "arguments", "port", "path"});
  auto exe = launch.at("executable").get<std::string>();
  if (exe.starts_with("{app_dir}/")) {
    const auto file = exe.substr(10); path_ok(file);
    const auto& files = dist.at("files");
    if (std::none_of(files.begin(), files.end(), [&](const auto& f) { return f.at("path") == file && f.value("executable", false); }))
      throw std::invalid_argument("bundled executable must be a verified executable package file");
  } else if (!std::regex_match(exe, std::regex("[A-Za-z0-9_.-]+"))) throw std::invalid_argument("launch executable must be a PATH command or verified {app_dir} file (no shell string)");
  if (!launch.at("arguments").is_array() || launch.at("arguments").size() > 64) throw std::invalid_argument("invalid launch arguments");
  for (const auto& arg : launch.at("arguments")) { auto s = arg.get<std::string>(); if (s.find('\0') != std::string::npos || s.size() > 4096) throw std::invalid_argument("invalid launch argument"); }
  if (launch.contains("port") || launch.contains("path")) {
    const int port = launch.at("port"); if (port < 1024 || port > 65535) throw std::invalid_argument("invalid app port");
    auto route = launch.at("path").get<std::string>(); if (!std::regex_match(route, std::regex("/[a-zA-Z0-9/_-]*"))) throw std::invalid_argument("invalid UI path");
  }
  if (!j.at("workloads").is_array()) throw std::invalid_argument("workloads must be an array");
  std::set<std::string> workload_ids;
  for (const auto& w : j.at("workloads")) {
    fields(w, {"id", "file", "description"}); path_ok(w.at("file"));
    if (!id_ok(w.at("id")) || !workload_ids.insert(w.at("id")).second || !paths.contains(w.at("file"))) throw std::invalid_argument("invalid bundled workload");
    if (w.at("description").get<std::string>().empty()) throw std::invalid_argument("workload needs description");
  }
}
void validate_app_index(const json& j) {
  fields(j, {"schema", "apps"});
  if (j.at("schema") != 1 || !j.at("apps").is_array() || j.at("apps").size() > 256) throw std::invalid_argument("invalid app index");
  std::set<std::string> ids;
  for (const auto& a : j.at("apps")) {
    fields(a, {"id", "name", "description", "manifest", "sha256"}); path_ok(a.at("manifest"));
    if (!id_ok(a.at("id")) || !ids.insert(a.at("id")).second || !hash_ok(a.at("sha256"))) throw std::invalid_argument("invalid app index entry");
    for (auto key : {"name", "description"}) if (a.at(key).get<std::string>().empty()) throw std::invalid_argument("empty app description/name");
  }
}
std::vector<std::string> active_app_workloads(const fs::path& root) {
  std::set<std::string> workloads;
  const auto path = root / "state/apps";
  if (!fs::is_directory(path) || fs::is_symlink(path)) return {};
  for (const auto& file : fs::directory_iterator(path)) {
    if (!file.is_regular_file() || file.is_symlink() || file.path().extension() != ".json") continue;
    try {
      const auto state = read_json(file.path());
      if (running(state) && !state.value("remote_connection", false) && state.contains("workload") && state["workload"].is_string())
        workloads.insert(state["workload"].get<std::string>());
    } catch (const std::exception&) { /* Stale/corrupt app records never block inference. */ }
  }
  return {workloads.begin(), workloads.end()};
}

json app_inventory(const fs::path& root, const fs::path& config) {
  json result = {{"sources", json::array()}, {"apps", json::array()}};
  const auto base = bundled(config); const auto index = base / "apps.yaml";
  if (fs::is_regular_file(index)) {
    const auto doc = read_profile_file(index); validate_app_index(doc);
    result["sources"].push_back({{"id", "mica-examples"}, {"name", "Mica demo apps"}, {"repository", "https://github.com/miguelamendez/mica-server"}, {"bundled", true}});
    for (auto app : doc.at("apps")) {
      app["source_id"] = "mica-examples"; app["bundled_path"] = (base / app.at("manifest").get<std::string>()).string();
      app["manifest_sha256"] = app.at("sha256"); result["apps"].push_back(app);
    }
  }
  auto sources = read_json(root / "config/app-sources.json");
  for (auto it = sources.begin(); it != sources.end(); ++it) {
    if (!id_ok(it.key())) throw std::runtime_error("invalid saved app source ID");
    result["sources"].push_back({{"id", it.key()}, {"repository", it.value().at("source")}, {"resolved_url", it.value().at("resolved_url")}});
    for (auto app : it.value().at("apps")) {
      app["source_id"] = it.key();
      app["id"] = it.key() + ":" + app.at("id").get<std::string>(); result["apps"].push_back(app);
    }
  }
  if (fs::is_directory(root / "apps")) for (const auto& directory : fs::directory_iterator(root / "apps")) {
    const auto id = directory.path().filename().string();
    if (!id_ok(id) || directory.is_symlink() || !directory.is_directory() || !fs::is_regular_file(directory.path() / "receipt.json")) continue;
    const bool listed = std::any_of(result["apps"].begin(), result["apps"].end(), [&](const auto& a) {
      auto name = a.at("id").template get<std::string>(); if (auto colon = name.find(':'); colon != std::string::npos) name.erase(0, colon + 1); return name == id;
    });
    if (!listed) {
      const auto doc = read_profile_file(directory.path() / "app.yaml"); validate_app_manifest(doc);
      result["apps"].push_back({{"id", id}, {"name", doc.at("name")}, {"description", doc.at("description")},
        {"source_id", read_json(directory.path() / "receipt.json").at("source_id")}});
    }
  }
  for (auto& app : result["apps"]) {
    // Remote catalogue IDs are namespaced in the UI; installation uses manifest ID.
    const auto display = app.at("id").get<std::string>(); auto id = display.substr(display.find(':') == std::string::npos ? 0 : display.find(':') + 1);
    const auto dir = root / "apps" / id;
    const auto receipt = read_json(dir / "receipt.json");
    const auto state = read_json(root / "state/apps" / (id + ".json"));
    app["installed"] = !receipt.empty(); app["running"] = running(state);
    if (state.contains("workload")) app["bound_workload"] = state["workload"];
    if (state.contains("url")) app["url"] = state["url"];
    const auto manifest_path=!receipt.empty()?dir/"app.yaml":app.contains("bundled_path")?fs::path(app.at("bundled_path").get<std::string>()):fs::path();
    if(!manifest_path.empty()&&fs::is_regular_file(manifest_path))app["interfaces"]=read_profile_file(manifest_path).value("interfaces",json::array());
    const auto connections = read_json(root / "config/app-connections" / (id + ".json"));
    if (!connections.empty()) {
      app["connections"] = json::object();
      const auto records=connections.value("connections",json::object());
      for (const auto& [name, connection] : records.items())
        app["connections"][name] = {{"url", connection.value("url", "")}, {"workload", connection.value("workload", "")}};
      app["default_connection"] = connections.value("default", "");
    }
    if (!receipt.empty()) { app["installed_source"] = receipt.value("source_id", ""); app["workloads"] = receipt.value("workloads", json::array()); }
  }
  return result;
}

int app_command(const std::vector<std::string>& arguments, const fs::path& default_root, const fs::path& default_config,
                const fs::path& executable) {
  if (arguments.empty()) throw std::invalid_argument("app needs list|show|source-add|source-remove|install|setup|start|stop|uninstall");
  auto root = default_root, config = default_config; std::vector<std::string> positional; bool accept = false;
  std::string workload_binding;
  std::string connection_name, connection_url, connection_key_file;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const auto& arg = arguments[i];
    if (arg == "--root" || arg == "--config-dir") { if (++i == arguments.size()) throw std::invalid_argument("missing app option value"); (arg == "--root" ? root : config) = arguments[i]; }
    else if (arg == "--accept-code") accept = true;
    else if (arg == "--workload") { if (++i == arguments.size()) throw std::invalid_argument("missing workload ID"); workload_binding = arguments[i]; }
    else if (arg == "--connection" || arg == "--url" || arg == "--api-key-file") {
      if (++i == arguments.size()) throw std::invalid_argument("missing connection option value");
      (arg == "--connection" ? connection_name : arg == "--url" ? connection_url : connection_key_file) = arguments[i];
    }
    else if (arg.starts_with("--")) throw std::invalid_argument("unknown app option: " + arg);
    else positional.push_back(arg);
  }
  root = fs::absolute(root); config = fs::absolute(config);
  const auto action = arguments[0];
  if (action == "list") { std::cout << app_inventory(root, config).dump(2) << '\n'; return 0; }
  if (positional.empty()) throw std::invalid_argument("app action requires an ID or source");
  Lock lock(root);
  if (action == "source-add") {
    if (positional.size() != 2 || !id_ok(positional[0]) || positional[0] == "mica-examples") throw std::invalid_argument("source-add needs ID URL (official source is reserved)");
    const auto scratch = root / "apps/.discovery";
    auto discovered = discovery(positional[1], scratch);
    auto sources = read_json(root / "config/app-sources.json"); sources[positional[0]] = std::move(discovered);
    save(root / "config/app-sources.json", sources); std::cout << "Source registered. No app code executed.\n"; return 0;
  }
  if (action == "source-remove") {
    auto sources = read_json(root / "config/app-sources.json"); if (!sources.erase(positional[0])) throw std::invalid_argument("unknown/removable source");
    save(root / "config/app-sources.json", sources); return 0;
  }
  auto entry = find_app(app_inventory(root, config), positional[0]);
  auto id = entry.at("id").get<std::string>(); if (auto colon = id.find(':'); colon != std::string::npos) id.erase(0, colon + 1);
  if (!id_ok(id)) throw std::invalid_argument("invalid app ID");
  const auto dir = root / "apps" / id, statepath = root / "state/apps" / (id + ".json");
  if (action == "show" && !fs::is_directory(dir)) {
    const auto scratch = root / "apps/.inspection"; fs::create_directories(scratch);
    const auto file = scratch / "app.yaml";
    if (entry.contains("bundled_path")) {
      plain_file(bundled(config), entry.at("manifest")); fs::copy_file(entry.at("bundled_path").get<std::string>(), file, fs::copy_options::overwrite_existing);
    } else { std::ofstream out(file); out << fetch_text(entry.at("manifest_url")); out.close(); }
    if (digest(file) != entry.at("manifest_sha256").get<std::string>()) throw std::runtime_error("manifest SHA-256 mismatch");
    auto doc = read_profile_file(file); validate_app_manifest(doc);
    if (doc.at("id") != id) throw std::runtime_error("manifest/index ID mismatch");
    doc["catalogue_id"] = positional[0]; doc["installed"] = false; std::cout << doc.dump(2) << '\n'; return 0;
  }
  if (action == "install" || action == "upgrade") {
    if (!accept) throw std::runtime_error("Review/trust the source and permissions, then install with --accept-code. Installation does not launch code.");
    const bool upgrading = action == "upgrade";
    const auto prior_receipt = read_json(dir / "receipt.json");
    if (upgrading) {
      if (prior_receipt.empty() || fs::is_symlink(dir)) throw std::runtime_error("install the app before upgrading");
      if (running(read_json(statepath))) throw std::runtime_error("close this app before upgrading; Mica and other apps keep running");
      if (prior_receipt.at("source_id") != entry.at("source_id")) throw std::runtime_error("upgrade must use the original publisher");
    } else if (fs::exists(dir)) throw std::runtime_error("app already installed; use upgrade to replace verified files after closing it");
    const auto stage = root / "apps" / (".staging-" + id);
    if (fs::exists(stage)) throw std::runtime_error("previous staging directory exists; inspect before retrying: " + stage.string());
    fs::create_directory(stage); chmod(stage.c_str(), 0700);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{stage};
    const auto manifest_file = stage / "app.yaml";
    fs::path local_base; std::string remote_base;
    if (entry.contains("bundled_path")) {
      const fs::path source = entry.at("bundled_path").get<std::string>(); plain_file(bundled(config), entry.at("manifest"));
      fs::copy_file(source, manifest_file); local_base = source.parent_path();
    } else {
      const auto url = entry.at("manifest_url").get<std::string>(); remote_base = url.substr(0, url.rfind('/') + 1);
      const auto text = fetch_text(url); std::ofstream f(manifest_file); f << text; f.close();
    }
    if (digest(manifest_file) != entry.at("manifest_sha256").get<std::string>()) throw std::runtime_error("app manifest SHA-256 mismatch; aborted");
    const auto doc = read_profile_file(manifest_file); validate_app_manifest(doc);
    if (doc.at("id") != id) throw std::runtime_error("app manifest/index ID mismatch");
    for (const auto& file : doc.at("distribution").at("files")) {
      const auto relative = file.at("path").get<std::string>(); const auto target = stage / relative; fs::create_directories(target.parent_path());
      if (!local_base.empty()) { plain_file(local_base, relative); fs::copy_file(local_base / relative, target); }
      else {
        const auto url = file.value("url", remote_base + relative); https(url);
        const auto result = run_command({"curl", "--fail", "--silent", "--show-error", "--location",
          "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "20", "--max-time", "300",
          "--max-filesize", std::to_string(file.at("size_bytes").get<std::uint64_t>()), "--output", target.string(), url}, true);
        if (result.exit_code) throw std::runtime_error("app file download failed: " + relative);
      }
      if (fs::file_size(target) != file.at("size_bytes").get<std::uint64_t>() || digest(target) != file.at("sha256").get<std::string>()) throw std::runtime_error("app file size/SHA-256 mismatch: " + relative);
      fs::permissions(target, file.value("executable", false) ? fs::perms::owner_all : fs::perms::owner_read | fs::perms::owner_write);
    }
    auto registry = load_registry(config); merge_custom_models(registry, root); merge_installed_profiles(registry, root);
    json workloads = json::array(); std::vector<std::pair<fs::path, json>> registrations;
    for (const auto& workload : doc.at("workloads")) {
      auto document = read_profile_file(stage / workload.at("file").get<std::string>());
      const auto name = id + "--" + workload.at("id").get<std::string>(); document["id"] = name;
      profile_from_document(registry, document); const auto path = root / "config/profiles" / (name + ".yaml");
      if (fs::exists(path) || registry.profiles.contains(name)) {
        if (!upgrading || std::find(prior_receipt.at("workloads").begin(),prior_receipt.at("workloads").end(),json(name))==prior_receipt.at("workloads").end()) throw std::runtime_error("bundled workload would overwrite existing workload: " + name);
        workloads.push_back(name);continue; // Existing workload edits and active bindings are preserved.
      }
      registrations.emplace_back(path, document); workloads.push_back(name);
    }
    json receipt = {{"source_id", entry.at("source_id")}, {"manifest_sha256", digest(manifest_file)}, {"workloads", workloads}, {"workload_hashes", upgrading ? prior_receipt.value("workload_hashes",json::object()) : json::object()}};
    std::vector<fs::path> written;
    try {
      for (const auto& [path, doc] : registrations) { write_profile_file(path, doc); written.push_back(path); receipt["workload_hashes"][path.stem().string()] = digest(path); }
      save(stage / "receipt.json", receipt);
      if (upgrading) {
        const auto backup = root / "apps" / (".previous-"+id+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::rename(dir,backup);try{fs::rename(stage,dir);}catch(...){fs::rename(backup,dir);throw;}
        std::cout << "Previous app files retained at " << backup << ". Existing workloads/connections were kept.\n";
      } else fs::rename(stage, dir);
    } catch (...) { for (const auto& path : written) { std::error_code e; fs::remove(path, e); } throw; }
    std::cout << "Installed " << id << "; bundled workloads registered. No engine/model download or launch yet.\n"; return 0;
  }
  if (!fs::is_directory(dir) || fs::is_symlink(dir)) throw std::runtime_error("install the app first");
  const auto doc = read_profile_file(dir / "app.yaml"); validate_app_manifest(doc);
  if (entry.at("source_id") != read_json(dir / "receipt.json").at("source_id"))
    throw std::runtime_error("this app ID is already installed from another source; select its original source or uninstall explicitly");
  auto state = read_json(statepath);
  const auto connections_path = root / "config/app-connections" / (id + ".json");
  if (action == "connection-select") {
    auto saved=read_json(connections_path);
    if(!id_ok(connection_name)||!saved.value("connections",json::object()).contains(connection_name))throw std::invalid_argument("unknown connection name");
    saved["default"]=connection_name;write_private_file(connections_path,saved.dump(2));
    std::cout << "Default connection selected; reopen this app to use it. Mica is unchanged.\n";return 0;
  }
  if (action == "connect") {
    // Keep endpoint origins credential/path-free; apps use standard-library TLS for HTTPS.
    if (!id_ok(connection_name) || !std::regex_match(connection_url, std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/?)")))
      throw std::invalid_argument("connect requires --connection NAME --url HTTP(S)_ORIGIN --api-key-file PATH --workload ID");
    if (workload_binding.empty() || connection_key_file.empty()) throw std::invalid_argument("connection needs workload and inference key file");
    const auto key_file = fs::absolute(connection_key_file);
    if (!fs::is_regular_file(key_file) || fs::is_symlink(key_file)) throw std::invalid_argument("private inference key file missing or symlinked");
    auto saved = read_json(connections_path); if (saved.empty()) saved = {{"schema", 1}, {"connections", json::object()}};
    saved["connections"][connection_name] = {{"url", connection_url}, {"api_key_file", key_file.string()}, {"workload", workload_binding}};
    saved["default"] = connection_name; write_private_file(connections_path, saved.dump(2));
    std::cout << "Connection saved; choose it with app start --connection " << connection_name << ". No local inference required.\n"; return 0;
  }
  if (action == "show") { auto shown = doc; shown["catalogue_id"] = positional[0]; shown["installed"] = true; shown["running"] = running(state); if (state.contains("url")) shown["url"] = state["url"]; shown["registered_workloads"] = read_json(dir / "receipt.json").at("workloads"); std::cout << shown.dump(2) << '\n'; return 0; }
  if (action == "stop") { if (running(state) && kill(state.at("pid").get<int>(), SIGTERM)) throw std::runtime_error("cannot stop app"); save(statepath, json::object()); std::cout << "App stopped; model server remains unchanged.\n"; return 0; }
  if (action == "setup") {
    const auto workloads = read_json(dir / "receipt.json").at("workloads");
    if (positional.size() != 2 || std::find(workloads.begin(), workloads.end(), json(positional[1])) == workloads.end()) throw std::invalid_argument("setup needs a bundled workload ID; inspect app show");
    auto registry=load_registry(config);merge_custom_models(registry,root);merge_installed_profiles(registry,root);
    const auto inspection=inspect_workload(registry,registry.profile(positional[1]),detect_hardware(),root);
    if(!inspection.value("compatible",false))throw std::invalid_argument("workload unsupported on this host: "+inspection.value("reason",""));
    return run_command({executable.string(), "workload", "install", positional[1], "--root", root.string(), "--config-dir", config.string()}).exit_code;
  }
  if (action == "uninstall") {
    if (running(state)) throw std::runtime_error("stop the app before uninstalling");
    const auto receipt = read_json(dir / "receipt.json");
    // Do not remove a workload currently active in the server.
    const auto active = local_server_status(root).value("profile", json::object()).value("name", "");
    const auto settings = load_server_configuration(root / "config/server.json");
    const auto default_workload = settings.value("default_workload", "");
    for (const auto& w : receipt.at("workloads")) if (w == active || w == default_workload)
      throw std::runtime_error("select another active/default workload before uninstalling");
    for (const auto& w : receipt.at("workloads")) {
      const auto name = w.get<std::string>(); if (!id_ok(name)) throw std::runtime_error("invalid receipt workload");
      const auto p = root / "config/profiles" / (name + ".yaml");
      if (fs::is_regular_file(p) && !fs::is_symlink(p) && digest(p) == receipt.at("workload_hashes").at(name).get<std::string>()) fs::remove(p);
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto retained = root / "apps" / (".removed-" + id + "-" + std::to_string(stamp)); fs::rename(dir, retained);
    std::cout << "App unregistered. Files retained at " << retained << "; edited workloads and shared models/engines kept.\n"; return 0;
  }
  if (action != "start") throw std::invalid_argument("unknown app action");
  if (!accept) throw std::runtime_error("Launching downloaded code requires --accept-code; review app show first");
  if (running(state)) throw std::runtime_error("app is already running");
  if (digest(dir / "app.yaml") != read_json(dir / "receipt.json").at("manifest_sha256").get<std::string>()) throw std::runtime_error("installed app manifest changed; reinstall/revalidate before launch");
  for (const auto& file : doc.at("distribution").at("files")) {
    const auto p = file.at("path").get<std::string>(); plain_file(dir, p);
    if (digest(dir / p) != file.at("sha256").get<std::string>()) throw std::runtime_error("installed app changed; revalidate package before launch");
  }
  const auto connections = read_json(connections_path);
  if (connection_name.empty()) connection_name = connections.value("default", "");
  fs::path client_config = root / "config/server.json";
  const bool remote_connection = !connection_name.empty();
  if (remote_connection) {
    if(!connections.value("connections",json::object()).contains(connection_name))throw std::invalid_argument("unknown app connection; add it with app connect first");
    const auto connection = connections.at("connections").at(connection_name);
    if (!std::regex_match(connection.value("url", ""), std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/?)"))) throw std::invalid_argument("invalid saved connection URL");
    workload_binding = connection.at("workload");
    client_config = root / "config/client-runtime" / (id + ".json");
    write_private_file(client_config, json{{"ui", {{"mica_url", connection.at("url")}}}, {"api_key_file", connection.at("api_key_file")}}.dump());
  } else if (!local_server_running(root)) throw std::runtime_error("start Mica with a compatible workload first, or add a remote connection with app connect");
  if (workload_binding.empty()) {
    auto registry = load_registry(config); merge_installed_profiles(registry, root);
    const auto hardware = detect_hardware();
    for (const auto& id : read_json(dir / "receipt.json").at("workloads"))
      if (inspect_workload(registry, registry.profile(id.get<std::string>()), hardware, root).value("compatible", false)) { workload_binding = id; break; }
  }
  if (!workload_binding.empty() && (workload_binding.size() > 128 ||
      !std::isalnum(static_cast<unsigned char>(workload_binding.front())) ||
      !std::all_of(workload_binding.begin(), workload_binding.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; })))
    throw std::invalid_argument("invalid app workload ID");
  if (!remote_connection) {
  auto registry=load_registry(config);merge_custom_models(registry,root);merge_installed_profiles(registry,root);
  if(workload_binding.empty())throw std::invalid_argument("no compatible bundled workload; supply a supported --workload");
  const auto inspection=inspect_workload(registry,registry.profile(workload_binding),detect_hardware(),root);
  if(!inspection.value("compatible",false))throw std::invalid_argument("workload unsupported on this host: "+inspection.value("reason",""));
  const auto models = local_server_request(root, "GET", "/v1/models" + (workload_binding.empty() ? std::string() : "?workload=" + workload_binding), json::object());
  std::set<std::string> operations;
  for (const auto& model : models.at("data")) for (const auto& endpoint : model.value("endpoint_contracts", json::array()))
    for (const auto& interaction : model.value("supported_interactions", json::array()))
      if (interaction.at("operation") == endpoint.at("operation")) operations.insert(endpoint.at("operation").get<std::string>());
  for (const auto& op : doc.at("requirements").at("operations")) if (!operations.contains(op.get<std::string>())) throw std::runtime_error("requested workload lacks required operation: " + op.get<std::string>());
  }
  const auto& launch = doc.at("launch"); auto program = launch.at("executable").get<std::string>();
  if (program.starts_with("{app_dir}/")) program = (dir / program.substr(10)).string();
  if (!command_exists(program)) throw std::runtime_error("app dependency is missing: " + program + " (Mica never installs arbitrary app dependencies silently)");
  std::vector<std::string> argv{program};
  for (const auto& raw : launch.at("arguments")) {
    auto arg = raw.get<std::string>();
    if (arg.find("{port}") != std::string::npos && !launch.contains("port")) throw std::invalid_argument("headless app cannot use {port}");
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{{"{app_dir}", dir.string()}, {"{mica_config}", client_config.string()}, {"{port}", std::to_string(launch.value("port", 0))}, {"{workload}", workload_binding}}) {
      std::size_t pos = 0; while ((pos = arg.find(key, pos)) != std::string::npos) { arg.replace(pos, key.size(), value); pos += value.size(); }
    }
    argv.push_back(arg);
  }
  std::vector<char*> native; for (auto& arg : argv) native.push_back(arg.data()); native.push_back(nullptr);
  fs::create_directories(root / "state/apps"); const auto log = root / "state/apps" / (id + ".log");
  const int output = open(log.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (output < 0) throw std::runtime_error("cannot open app log");
  const auto pid = fork();
  if (pid == 0) { setsid(); chdir(dir.c_str()); dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO); close(output); const int input = open("/dev/null", O_RDONLY); dup2(input, STDIN_FILENO); close(input); execvp(native[0], native.data()); _exit(127); }
  close(output); if (pid < 0) throw std::runtime_error("cannot launch app");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto started = birth(pid); if (started.empty()) { waitpid(pid, nullptr, WNOHANG); throw std::runtime_error("app exited during launch; inspect its private log"); }
  state = {{"pid", pid}, {"birth", started}, {"workload", workload_binding}, {"remote_connection", remote_connection}};
  if (launch.contains("port")) state["url"] = "http://127.0.0.1:" + std::to_string(launch.at("port").get<int>()) + launch.at("path").get<std::string>();
  save(statepath, state); std::cout << "App process launched: " << state.value("url", "headless") << " (check app list/log for readiness).\n"; return 0;
}

fs::path export_app_client(const fs::path& root, const fs::path& config, const fs::path& executable,
                          const std::string& catalogue_id, const std::string& server_url,
                          const std::string& workload, const std::string& mode) {
  if (mode != "remote" && mode != "local") throw std::invalid_argument("client inference mode must be remote or local");
  if (!std::regex_match(server_url, std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/?)"))) throw std::invalid_argument("invalid server origin");
  const auto item = find_app(app_inventory(root, config), catalogue_id);
  auto id = item.at("id").get<std::string>(); if (auto colon=id.find(':'); colon!=std::string::npos) id.erase(0,colon+1);
  if (!id_ok(id)) throw std::invalid_argument("invalid app ID");
  const auto scratch = root / "state/client-downloads" / generate_api_key().substr(0,24);
  fs::create_directories(scratch);
  struct Cleanup { fs::path path; ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);} } cleanup{scratch};
  if (fs::is_regular_file(root/"config/app-sources.json")) write_private_file(scratch/"config/app-sources.json",read_json(root/"config/app-sources.json").dump());
  // Reuse the verified installer in an isolated staging home, never change the host's installed apps.
  app_command({"install", catalogue_id, "--accept-code"}, scratch, config, executable);
  const auto source = scratch / "apps" / id;
  auto package = read_profile_file(source / "app.yaml");
  if (package.at("launch").at("executable") != "python3")
    throw std::invalid_argument("device downloads currently support portable Python launchers only; no native installer supplied by publisher");
  const auto output = scratch / (id + "-client"); fs::create_directory(output);
  auto files = package.at("distribution").at("files");
  for (const auto& file : files) {
    const auto relative=file.at("path").get<std::string>(); plain_file(source,relative);
    fs::create_directories((output/relative).parent_path()); fs::copy_file(source/relative,output/relative);
  }
  for (const auto& [from,to] : std::vector<std::pair<fs::path,std::string>>{{config.parent_path()/"apps/client/client.py","client.py"},
      {config.parent_path()/"scripts/run_memory_limited.py","build_guard.py"}}) {
    if (!fs::is_regular_file(from) || fs::is_symlink(from)) throw std::runtime_error("client tools missing; install matching shared files");
    fs::copy_file(from, output/to);
    files.push_back({{"path",to},{"sha256",digest(output/to)},{"size_bytes",fs::file_size(output/to)}});
  }
  package["files"]=files; package["suggested_server"]=server_url; package["suggested_workload"]=workload; package["inference_mode"]=mode;
  write_private_file(output/"client.json",package.dump(2));
  const std::string readme="Mica standalone client (Python 3 standard library; no pip).\n\n"
    "1. Extract this archive, open a terminal in this folder.\n2. Run: python3 client.py install\n"
    "3. Remote inference: python3 client.py connect\n   Enter an INFERENCE key privately when prompted.\n"
    "   Local inference instead: python3 client.py setup-local\n"
    "4. Run: python3 client.py start\n\n"
    "Add another server: python3 client.py connect --connection NAME --url http://HOST:PORT --workload ID\n"
    "Choose a server: python3 client.py start --connection NAME\n"
    "List connections: python3 client.py connections\n"
    "Existing installations/data are preserved. Local builds require CMake, Git, a C++20 compiler, Lua headers;\n"
    "builds are memory-capped. Nothing installs or runs until you execute these commands and approve setup.\n"
    "This package contains no API keys. Use HTTPS or a trusted encrypted network for remote inference.\n"
    "Selected setup: "+mode+". Suggested server: "+server_url+". Workload: "+workload+".\n";
  write_private_file(output/"README.txt",readme);
  const auto archive = root / "state/client-downloads" / (scratch.filename().string()+".tar.gz");
  const auto result=run_command({"tar","-czf",archive.string(),"-C",scratch.string(),output.filename().string()},true);
  if (result.exit_code) throw std::runtime_error("client archive creation failed (tar required)");
  fs::permissions(archive,fs::perms::owner_read|fs::perms::owner_write); return archive;
}
}

#include "mica_server/dashboard.hpp"
#include "mica_server/tui.hpp"
#include "mica_server/service.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/config.hpp"
#include "mica_server/catalog.hpp"
#include "mica_server/setup.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/command.hpp"
#include "mica_server/apps.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <tuple>
#include <condition_variable>
#include <deque>
#include <regex>
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
// A bounded streaming bridge: wait for upstream headers before sending status,
// then forward chunks without accumulating a whole SSE/audio response in RAM.
struct AppBridge {
  httplib::Client client;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::string> chunks;
  std::thread reader;
  httplib::Headers headers;
  std::size_t queued{0};
  int status{502};
  bool ready{false}, finished{false}, cancelled{false};
  explicit AppBridge(int port) : client("127.0.0.1",port) {
    client.set_connection_timeout(5,0); client.set_read_timeout(3600,0); client.set_follow_location(false);
  }
  ~AppBridge() {
    { std::lock_guard lock(mutex); cancelled=true; }
    changed.notify_all(); client.stop(); if(reader.joinable()) reader.join();
  }
  void start(httplib::Request request) {
    request.response_handler=[this](const httplib::Response& response) {
      std::lock_guard lock(mutex); status=response.status; headers=response.headers; ready=true; changed.notify_all(); return !cancelled;
    };
    request.content_receiver=[this](const char* data,std::size_t size,std::size_t,std::size_t) {
      std::unique_lock lock(mutex); changed.wait(lock,[&]{return cancelled||queued<262144;});
      if(cancelled)return false; chunks.emplace_back(data,size); queued+=size; changed.notify_all(); return true;
    };
    reader=std::thread([this,request=std::move(request)]{
      try { (void)client.send(request); } catch(...) {}
      { std::lock_guard lock(mutex); finished=true; ready=true; } changed.notify_all();
    });
  }
};
struct AppAccess {
  struct Session { std::string authorization, app; std::chrono::steady_clock::time_point expires; };
  std::mutex mutex;
  std::map<std::string,Session> sessions;
};
json running_app(const ServerOptions& options,const std::string& id) {
  const auto inventory=app_inventory(options.root,options.config_directory);
  for(const auto& app:inventory.at("apps")) {
    auto plain=app.at("id").get<std::string>(); if(auto colon=plain.find(':');colon!=std::string::npos)plain.erase(0,colon+1);
    if(plain==id && app.value("installed",false) && app.value("running",false))return app;
  }
  throw std::invalid_argument("app is not running on this host");
}
std::string cookie_value(const httplib::Request& request,const std::string& name) {
  const auto cookies=request.get_header_value("Cookie"); std::size_t begin=0;
  while(begin<cookies.size()) {
    while(begin<cookies.size()&&(cookies[begin]==' '||cookies[begin]==';'))++begin;
    const auto end=cookies.find(';',begin); const auto part=cookies.substr(begin,end-begin);
    if(part.starts_with(name+"="))return part.substr(name.size()+1);
    if(end==std::string::npos)break; begin=end+1;
  }
  return {};
}
std::string lower(std::string text) {std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return std::tolower(c);});return text;}
std::string app_html(std::string html,const std::string& prefix) {
  for(const auto& attr:{"src=\"/","href=\"/"}) {
    std::size_t p=0;const std::string needle=attr;
    while((p=html.find(needle,p))!=std::string::npos) { const auto start=p+needle.size()-1;
      if(start+1<html.size()&&html[start+1]!='/'&&html.compare(start,prefix.size(),prefix)!=0){html.insert(start,prefix);p=start+prefix.size()+1;}else p=start+1;
    }
  }
  const std::string script="<script>(()=>{const base="+json(prefix).dump()+";const original=window.fetch.bind(window);window.fetch=(input,options)=>{if(typeof input==='string'&&input.startsWith('/')&&!input.startsWith('//')&&!input.startsWith(base+'/'))input=base+input;return original(input,options)};})();</script>";
  const auto at=html.find("<script");html.insert(at==std::string::npos?0:at,script);return html;
}
void validate_job(const json& request) {
  if (!request.is_object() || !request.contains("action") || !request["action"].is_string()) throw std::invalid_argument("action required");
  static const std::map<std::string, std::set<std::string>> fields{
    {"workload.prepare", {"id"}}, {"workload.save", {"id", "yaml"}}, {"workload.clone", {"id", "new_id"}},
    {"app.install", {"id", "accept_code"}}, {"app.upgrade", {"id", "accept_code"}}, {"app.start", {"id", "workload", "accept_code"}},
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
  if (action == "app.install" || action == "app.upgrade" || action == "app.start") if (!request.value("accept_code", false)) throw std::invalid_argument("explicit trusted-code consent required");
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
    if (action == "app.install" || action == "app.upgrade" || action == "app.start") command.push_back("--accept-code");
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
  auto app_access=std::make_shared<AppAccess>();
  server.Post(R"(/admin/dashboard/apps/([A-Za-z0-9_.-]+)/open)",[auth,options,app_access](const auto& request,auto& response){
    if(!auth(request,response))return;
    try {
      const auto id=request.matches[1].str();const auto app=running_app(options,id);
      const auto local=app.at("url").template get<std::string>();std::smatch match;
      if(!std::regex_match(local,match,std::regex(R"(http://127\.0\.0\.1:([0-9]{1,5})(/[^?#]*))")))throw std::invalid_argument("only loopback HTTP browser apps can be opened");
      const auto prefix="/apps/"+id;const auto token=generate_api_key();
      { std::lock_guard lock(app_access->mutex);const auto now=std::chrono::steady_clock::now();
        std::erase_if(app_access->sessions,[&](const auto& item){return item.second.expires<=now;});
        if(app_access->sessions.size()>=1024)throw std::runtime_error("too many app sessions");
        app_access->sessions[token]={request.get_header_value("Authorization"),id,now+std::chrono::hours(8)};
      }
      response.set_header("Set-Cookie","mica_app="+token+"; Path="+prefix+"/; HttpOnly; SameSite=Strict; Max-Age=28800");
      response.set_content(json{{"url",prefix+match[2].str()},{"host",request.get_header_value("Host")}}.dump(),"application/json");
    }catch(const std::exception& error){response.status=400;response.set_content(json{{"error",error.what()}}.dump(),"application/json");}
  });
  server.Post("/admin/dashboard/apps/logout",[auth,app_access](const auto& request,auto& response){
    if(!auth(request,response))return;
    std::lock_guard lock(app_access->mutex);const auto authorization=request.get_header_value("Authorization");
    std::erase_if(app_access->sessions,[&](const auto& item){return item.second.authorization==authorization;});
    response.set_content("{\"ok\":true}","application/json");
  });
  server.Post(R"(/admin/dashboard/apps/([A-Za-z0-9_:-]+)/download)",[auth,options](const auto& request,auto& response){
    if(!auth(request,response))return;
    fs::path archive;
    try {
      if(request.body.size()>4096)throw std::invalid_argument("download request too large");
      const auto data=json::parse(request.body);
      for(const auto& [key,_]:data.items())if(key!="mode"&&key!="server_url"&&key!="workload")throw std::invalid_argument("unexpected download field");
      archive=export_app_client(options.root,options.config_directory,options.executable,request.matches[1].str(),
          data.at("server_url"),data.value("workload",""),data.at("mode"));
      response.set_header("Content-Disposition","attachment; filename=\"mica-app-client.tar.gz\"");
      response.set_content(contents(archive),"application/gzip");fs::remove(archive);
    }catch(const std::exception& error){if(!archive.empty()){std::error_code ec;fs::remove(archive,ec);}response.status=400;response.set_content(json{{"error",error.what()}}.dump(),"application/json");}
  });
  const auto app_proxy=[options,credentials,app_access](const httplib::Request& request,httplib::Response& response){
    const auto id=request.matches[1].str(),prefix="/apps/"+id;
    const auto origin=request.get_header_value("Origin"),authority=request.get_header_value("Host");
    if((!origin.empty()&&origin!="http://"+authority&&origin!="https://"+authority)||request.get_header_value("Sec-Fetch-Site")=="cross-site"){
      response.status=403;response.set_content("{\"error\":\"origin_rejected\"}","application/json");return;
    }
    bool accepted=false;{std::lock_guard lock(app_access->mutex);auto it=app_access->sessions.find(cookie_value(request,"mica_app"));
      if(it!=app_access->sessions.end()&&it->second.app==id&&it->second.expires>std::chrono::steady_clock::now())accepted=credentials->allows(it->second.authorization,true);
    }
    if(!accepted){response.status=401;response.set_content("App access expired. Open this app again from the Mica dashboard.","text/plain");return;}
    try {
      const auto app=running_app(options,id);std::smatch match;const auto local=app.at("url").get<std::string>();
      if(!std::regex_match(local,match,std::regex(R"(http://127\.0\.0\.1:([0-9]{1,5})(/[^?#]*))")))throw std::invalid_argument("app address is not private loopback");
      const int port=std::stoi(match[1]);if(port<1024||port>65535)throw std::invalid_argument("invalid app port");
      auto suffix=request.path.substr(prefix.size());if(suffix.empty())suffix="/";
      if(suffix.find("..")!=std::string::npos||suffix.find('\\')!=std::string::npos)throw std::invalid_argument("invalid app path");
      httplib::Request upstream;upstream.method=request.method;upstream.path=suffix;
      if(const auto query=request.target.find('?');query!=std::string::npos)upstream.path+=request.target.substr(query);
      for(const auto& [name,value]:request.headers) {
        const auto key=lower(name);
        if(key=="accept"||key=="content-type"||key=="range")upstream.headers.emplace(name,value);
      }
      upstream.headers.emplace("Origin","http://127.0.0.1:"+std::to_string(port));
      upstream.body=request.body;
      if(request.is_multipart_form_data()) {
        const auto boundary="mica-"+generate_api_key();upstream.headers.erase("Content-Type");
        upstream.headers.emplace("Content-Type","multipart/form-data; boundary="+boundary);upstream.body.clear();
        const auto quote=[](std::string text){for(auto& c:text)if(c=='\r'||c=='\n'||c=='"')c='_';return text;};
        for(const auto& [name,field]:request.form.fields)upstream.body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+quote(name)+"\"\r\n\r\n"+field.content+"\r\n";
        for(const auto& [name,file]:request.form.files)upstream.body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+quote(name)+"\"; filename=\""+quote(file.filename)+"\"\r\nContent-Type: application/octet-stream\r\n\r\n"+file.content+"\r\n";
        upstream.body+="--"+boundary+"--\r\n";
      }
      if(upstream.body.size()>64*1024*1024){response.status=413;response.set_content("App upload exceeds 64 MiB","text/plain");return;}
      const auto bridge=std::make_shared<AppBridge>(port);bridge->start(std::move(upstream));
      {std::unique_lock lock(bridge->mutex);if(!bridge->changed.wait_for(lock,std::chrono::seconds(10),[&]{return bridge->ready;}))throw std::runtime_error("app did not respond");
        response.status=bridge->status;
        for(const auto& [name,value]:bridge->headers) {
          const auto key=lower(name);if(key=="content-type"||key=="content-disposition"||key=="content-range"||key=="accept-ranges")response.set_header(name,value);
          if(key=="location"&&value.starts_with('/')&&!value.starts_with("//"))response.set_header("Location",prefix+value);
        }
      }
      response.set_header("Cache-Control","no-store");response.set_header("X-Content-Type-Options","nosniff");response.set_header("Referrer-Policy","no-referrer");
      response.set_header("Content-Security-Policy","frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
      const auto mime=response.get_header_value("Content-Type");
      if(mime.starts_with("text/html")) {
        std::string html;std::unique_lock lock(bridge->mutex);
        for(;;){bridge->changed.wait(lock,[&]{return bridge->finished||!bridge->chunks.empty();});
          while(!bridge->chunks.empty()){html+=bridge->chunks.front();bridge->queued-=bridge->chunks.front().size();bridge->chunks.pop_front();bridge->changed.notify_all();}
          if(html.size()>4*1024*1024)throw std::runtime_error("app HTML too large");if(bridge->finished)break;
        }
        lock.unlock();response.set_content(app_html(std::move(html),prefix),mime);
      }else {
        response.set_chunked_content_provider(mime.empty()?"application/octet-stream":mime,[bridge](std::size_t,httplib::DataSink& sink){
          std::unique_lock lock(bridge->mutex);bridge->changed.wait(lock,[&]{return bridge->finished||!bridge->chunks.empty();});
          if(bridge->chunks.empty()){sink.done();return true;}
          auto chunk=std::move(bridge->chunks.front());bridge->chunks.pop_front();bridge->queued-=chunk.size();bridge->changed.notify_all();lock.unlock();return sink.write(chunk.data(),chunk.size());
        });
      }
    }catch(const std::exception& error){response.status=502;response.set_content(json{{"error",error.what()}}.dump(),"application/json");}
  };
  server.Get(R"(/apps/([A-Za-z0-9_.-]+)(/.*)?)",app_proxy);
  server.Post(R"(/apps/([A-Za-z0-9_.-]+)(/.*)?)",app_proxy);
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

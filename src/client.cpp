#include "mica_server/client.hpp"
#include "mica_server/apps.hpp"
#include "mica_server/build_guard.hpp"
#include "mica_server/command.hpp"
#include "mica_server/credentials.hpp"
#include "mica_server/ports.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/app_http.hpp"
#include "mica_server/service.hpp"
#include "mica_server/setup.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <httplib.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <termios.h>
#include <unistd.h>

namespace mica {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
std::string text_file(const fs::path& p) {
  std::ifstream stream(p, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read " + p.string());
  return {std::istreambuf_iterator<char>(stream), {}};
}
json read(const fs::path& p) { return fs::exists(p) ? json::parse(text_file(p)) : json::object(); }
bool id_ok(const std::string& id) { return std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,79}")); }
std::string plain_id(const std::string& id) { return id.substr(id.find(':') == std::string::npos ? 0 : id.find(':') + 1); }
fs::path binary(const ClientOptions& o) {
  auto sibling = o.executable.parent_path() / "mica-server";
  if (access(sibling.c_str(), X_OK) == 0) return sibling;
  const char* raw = std::getenv("PATH"); std::istringstream paths(raw ? raw : ""); std::string part;
  while (std::getline(paths, part, ':')) {
    auto candidate = fs::path(part) / "mica-server";
    if (access(candidate.c_str(), X_OK) == 0) return fs::absolute(candidate);
  }
  if(const auto home=std::getenv("HOME")){
    const auto candidate=fs::path(home)/".local/bin/mica-server";
    if(access(candidate.c_str(),X_OK)==0)return candidate;
  }
  return {};
}
std::string origin(const std::string& url) {
  // Origins only. No credentials, paths, query parameters, fragments or redirects.
  std::smatch m;
  if (!std::regex_match(url, m, std::regex(R"((https?)://([A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])(?::([0-9]{1,5}))?/?)")))
    throw std::invalid_argument("Use a credential-free HTTP(S) server origin, e.g. http://HOST:55000");
  if (m[3].matched && (std::stoi(m[3]) < 1 || std::stoi(m[3]) > 65535)) throw std::invalid_argument("Invalid server port");
  auto result = url; if (result.back() == '/') result.pop_back(); return result;
}
json remote_get(const ClientOptions& o, const std::string& url, const std::string& path, const fs::path& key = {}) {
  const auto address = origin(url);
  const auto scratch = o.root / "state" / ("request-" + generate_api_key().substr(0, 20));
  fs::create_directories(scratch);
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p,ec); } } cleanup{scratch};
  std::vector<std::string> cmd{"curl", "--fail", "--silent", "--show-error", "--proto", "=http,https",
    "--connect-timeout", "3", "--max-time", "20", "--max-filesize", "2097152"};
  if (!key.empty()) {
    if (!fs::is_regular_file(key) || fs::is_symlink(key)) throw std::invalid_argument("Missing private inference-key file");
    auto value = text_file(key); while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (value.size() < 16 || value.size() > 512 || value.find_first_of("\r\n") != std::string::npos)
      throw std::invalid_argument("Invalid inference key");
    const auto header = scratch / "headers";
    write_private_file(header, "Authorization: Bearer " + value + "\n");
    cmd.insert(cmd.end(), {"--header", "@" + header.string()});
  }
  cmd.push_back(address + path);
  auto r = run_command(cmd, true);
  if (r.exit_code || r.output.size() > 2097152) throw std::runtime_error("Cannot reach server / operation unavailable. Check URL, inference key and workload.");
  return json::parse(r.output);
}
json connection_view(const ClientOptions& o) {
  auto saved = read(o.root / "config/connections.json");
  for (auto& [name, record] : saved.items()) { (void)name; record.erase("api_key_file"); }
  return saved;
}
json app_manifest(const ClientOptions& o, const std::string& id) {
  if (!id_ok(plain_id(id))) throw std::invalid_argument("Invalid app ID");
  auto p = o.root / "apps" / plain_id(id) / "app.yaml";
  if (!fs::is_regular_file(p)) throw std::runtime_error("Install the app first");
  auto doc = read_profile_file(p); validate_app_manifest(doc); return doc;
}
void compatible_connection(const ClientOptions& o, const std::string& id, const json& connection) {
  auto health = remote_get(o, connection.at("url"), "/health");
  if (health.value("status", "") != "ok" || !health.contains("version")) throw std::runtime_error("Not a healthy Mica server");
  auto workload = connection.at("workload").get<std::string>();
  if (workload.empty() || !std::regex_match(workload,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}"))) throw std::invalid_argument("Invalid workload ID");
  auto models = remote_get(o, connection.at("url"), "/v1/models?workload=" + workload,
                           connection.at("api_key_file").get<std::string>());
  std::set<std::string> operations;
  for (const auto& model : models.at("data"))
    for (const auto& endpoint : model.value("endpoint_contracts", json::array()))
      for (const auto& interaction : model.value("supported_interactions", json::array()))
        if (endpoint.at("operation") == interaction.at("operation")) operations.insert(endpoint.at("operation"));
  const auto manifest=app_manifest(o,id);
  for (const auto& op : manifest.at("requirements").at("operations"))
    if (!operations.contains(op.get<std::string>())) throw std::runtime_error("Server workload lacks required operation: " + op.get<std::string>());
}
void bind_app_connection(const ClientOptions& o, const std::string& id, const std::string& name) {
  const auto connections = read(o.root / "config/connections.json");
  if (!connections.contains(name)) throw std::invalid_argument("Unknown server connection; add one first");
  const auto& c = connections.at(name); compatible_connection(o, id, c);
  const int status = app_command({"connect", id, "--connection", name, "--url", c.at("url"),
    "--api-key-file", c.at("api_key_file"), "--workload", c.at("workload")}, o.root, o.config, binary(o));
  if (status) throw std::runtime_error("Cannot bind app connection");
}
void server_install(const ClientOptions& o) {
  if (!binary(o).empty()) return;
  auto stage = o.root / "state" / ("server-source-" + generate_api_key().substr(0, 16));
  fs::create_directories(stage);
  auto result = run_command({"git", "clone", "--depth", "1", "https://github.com/miguelamendez/mica-server.git", (stage/"source").string()},true);
  if (result.exit_code) throw std::runtime_error("Cannot download official source; staging kept at " + stage.string());
  const auto budget = choose_build_budget(live_memory_snapshot(), 8, 1, 1);
  for (const auto& command : std::vector<std::vector<std::string>>{
    {"cmake","-S",(stage/"source").string(),"-B",(stage/"build").string(),"-DCMAKE_BUILD_TYPE=Release","-DMICA_SERVER_BUILD_TESTS=OFF"},
    {"cmake","--build",(stage/"build").string(),"--parallel","1"}}) {
    if (run_build_guarded(command, budget,true).exit_code) throw std::runtime_error("Server build failed; CMake, Git, C++20 and Lua development files required. Inspect " + stage.string());
  }
  const auto home = std::getenv("HOME"); if (!home) throw std::runtime_error("HOME missing");
  if (run_command({"cmake","--install",(stage/"build").string(),"--prefix",(fs::path(home)/".local").string()},true).exit_code)
    throw std::runtime_error("Server install failed; staging retained");
}
json local_setup(const ClientOptions& o, const json& request) {
  const auto id=request.at("id").get<std::string>(); app_manifest(o,id);
  if (!request.value("accept_downloads",false)) throw std::runtime_error("Local setup may download engines/models and start inference. Explicit --accept-downloads required.");
  if (binary(o).empty()) {
    if(!request.value("install_server",false))throw std::runtime_error("Mica Server is not installed. Review build prerequisites, then explicitly choose --install-server.");
    server_install(o);
  }
  const auto server=binary(o); if(server.empty())throw std::runtime_error("Installed server not found on PATH or beside mica-client; reopen your shell");
  const auto connection_name=request.value("name",std::string("local"));
  if(!id_ok(connection_name))throw std::invalid_argument("Invalid connection name");
  const auto id_plain=plain_id(id);
  // Register the verified package's workloads without installing another copy of the app on the server.
  const auto receipt=read(o.root/"apps"/id_plain/"receipt.json");
  auto workload=request.value("workload",std::string());
  if(workload.empty())throw std::invalid_argument("Choose a bundled workload from app show; hardware compatibility is validated by Mica setup");
  if(std::find(receipt.at("workloads").begin(),receipt.at("workloads").end(),json(workload))==receipt.at("workloads").end())
    throw std::invalid_argument("Local setup requires a workload bundled with this app");
  auto doc=read_profile_file(o.root/"config/profiles"/(workload+".yaml"));
  const bool first_machine=!fs::exists(o.server_root/"config/machine.yaml");
  const auto requirements=doc.value("memory",json::object());
  std::vector<std::string> prepare{"workload","install",workload};
  if(first_machine){
    // An initial allocation is explicit in the selected workload and confirmed
    // by the setup UI; never silently increase an existing machine policy.
    const auto ram=requirements.value("required_ram_gib",8.0),vram=requirements.value("required_vram_gib",0.0);
    prepare.insert(prepare.end(),{"--ram-gib",std::to_string(ram),"--vram-gib",std::to_string(vram)});
  }
  const auto destination=o.server_root/"config/profiles"/(workload+".yaml");
  if(fs::exists(destination)) {
    if(read_profile_file(destination)!=doc)throw std::runtime_error("Local workload already exists with different settings; refusing to overwrite");
  } else write_profile_file(destination,doc);
  auto run=[&](std::vector<std::string> args){args.insert(args.begin(),server.string());args.insert(args.end(),{"--root",o.server_root.string(),"--config-dir",o.config.string()});
    if(run_command(args,true).exit_code)throw std::runtime_error("Local setup failed; run mica-server workload install "+workload+" to inspect hardware/memory requirements");};
  const bool running=local_server_running(o.server_root);
  // Live installation uses the server's drain/maintenance queue, never a blind restart.
  run(prepare);
  if(!running)run({"start","--workload",workload});
  auto status=local_server_status(o.server_root);
  if(!status.contains("api_url"))throw std::runtime_error("Local server not ready");
  auto settings=load_server_configuration(o.server_root/"config/server.json");
  auto key=settings.value("api_key_file",(o.server_root/"secrets/api-key").string());
  auto saved=read(o.root/"config/connections.json");
  saved[connection_name]={{"url",status.at("api_url")},{"api_key_file",key},{"workload",workload}};
  compatible_connection(o,id,saved.at(connection_name));
  write_private_file(o.root/"config/connections.json",saved.dump(2)); bind_app_connection(o,id,connection_name);
  return {{"status","connected"},{"connection",connection_name},{"server_url",status.at("api_url")}};
}
json inventory(const ClientOptions& o) {
  return {{"apps",app_inventory(o.root,o.config)},{"servers",connection_view(o)},
          {"local_server",client_detect(o)},{"root",o.root.string()}};
}
std::string ask(const std::string& prompt, bool secret=false) {
  std::cout<<prompt<<": "<<std::flush;
  termios old{}; const bool hide=secret&&isatty(STDIN_FILENO)&&tcgetattr(STDIN_FILENO,&old)==0;
  if(hide){auto current=old;current.c_lflag&=~ECHO;tcsetattr(STDIN_FILENO,TCSAFLUSH,&current);}
  std::string value;std::getline(std::cin,value);
  if(hide){tcsetattr(STDIN_FILENO,TCSAFLUSH,&old);std::cout<<'\n';}return value;
}
void wizard(const ClientOptions& o,const std::string& id) {
  const auto mode=ask("Inference: local / remote / later");
  if(mode=="later"||mode.empty())return;
  if(mode=="remote") {
    auto saved=connection_view(o);std::cout<<saved.dump(2)<<'\n';
    const auto name=ask("Existing connection name, or a new name");
    if(!saved.contains(name)) {
      const auto url=ask("Server origin (http://HOST:55000)"),workload=ask("Workload ID"),key=ask("Inference key",true);
      client_action(o,{{"action","connection.add"},{"name",name},{"url",url},{"workload",workload},{"api_key",key}});
    }
    client_action(o,{{"action","app.bind"},{"id",id},{"name",name}});return;
  }
  if(mode!="local")throw std::invalid_argument("Choose local, remote or later");
  std::cout<<client_detect(o).dump(2)<<'\n';
  auto doc=app_manifest(o,id);std::cout<<doc.at("workloads").dump(2)<<'\n';
  const auto suffix=ask("Bundled workload suffix (e.g. mac or cuda)");
  const auto workload=plain_id(id)+"--"+suffix;
  if(fs::is_regular_file(o.root/"config/profiles"/(workload+".yaml"))){
    auto selected=read_profile_file(o.root/"config/profiles"/(workload+".yaml"));
    std::cout<<"Selected workload memory requirements: "<<selected.value("memory",json::object()).dump(2)<<'\n';
    std::cout<<"A new server uses these initial allocations; existing machine limits are preserved. Hardware compatibility is validated before downloads.\n";
  }
  const bool missing=binary(o).empty();
  if(missing&&ask("Install local server from official source? Requires CMake, Git, C++20 and Lua development files [yes/no]")!="yes")return;
  if(ask("Prepare compatible engines/models and start local inference? Downloads may be many GB; workload plan estimates memory [yes/no]")!="yes")return;
  std::cout<<local_setup(o,{{"id",id},{"workload",workload},{"accept_downloads",true},{"install_server",missing}}).dump(2)<<'\n';
}
int tui(const ClientOptions& o) {
  using namespace ftxui;
  if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO))throw std::runtime_error("mica-client tui needs an interactive terminal");
  std::string title="Mica Client",subtitle="Apps on this device · inference on your chosen server";
  lua_State* lua=luaL_newstate();
  // Presentation config gets no standard libraries or executable application policy.
  if(lua&&luaL_dofile(lua,(o.config/"client.lua").c_str())==LUA_OK&&lua_istable(lua,-1)) {
    lua_getfield(lua,-1,"title");if(lua_isstring(lua,-1))title=lua_tostring(lua,-1);lua_pop(lua,1);
    lua_getfield(lua,-1,"subtitle");if(lua_isstring(lua,-1))subtitle=lua_tostring(lua,-1);lua_pop(lua,1);
  }
  if(lua)lua_close(lua);
  auto screen=ScreenInteractive::Fullscreen();int tab=0,index=0;std::string message;
  std::vector<std::string> tabs{"Installed","Discover","Servers"},rows;
  json data=inventory(o);
  auto update=[&]{rows.clear();if(tab==2){for(auto& [name,c]:data["servers"].items()){(void)c;rows.push_back(name);}}
    else for(auto& a:data["apps"]["apps"])if(tab==1||a.value("installed",false))rows.push_back(a.at("id"));
    index=std::clamp(index,0,std::max(0,int(rows.size())-1));};update();
  auto menu=Menu(&rows,&index);
  auto tabs_ui=Toggle(&tabs,&tab);
  auto layout=Container::Vertical({tabs_ui,menu});
  auto view=Renderer(layout,[&]{update();Elements detail;
    if(!rows.empty()){
      if(tab==2)detail.push_back(paragraph(data["servers"].at(rows[index]).dump(2)));
      else for(const auto& a:data["apps"]["apps"])if(a.at("id")==rows[index]){
        detail.push_back(text(a.value("name",rows[index]))|bold);detail.push_back(paragraph(a.value("description","")));
        detail.push_back(text(a.value("installed",false)?"Installed on this device":"Not installed"));
        detail.push_back(paragraph("Connections: "+a.value("connections",json::object()).dump()));
      }
    }
    return vbox({text(title)|bold,paragraph(subtitle),tabs_ui->Render(),separator(),
      hbox({menu->Render()|size(WIDTH,EQUAL,30),separator(),vbox(std::move(detail))|flex})|flex,
      separator(),paragraph("i install · c configure local/remote · o open · s background · x close · d uninstall · a add source · r refresh · q quit"),paragraph(message)})|border;});
  auto component=CatchEvent(view,[&](Event e){
    if(e==Event::Character("q")){screen.ExitLoopClosure()();return true;}
    if(e==Event::Character("r")){data=inventory(o);update();return true;}
    if(e==Event::Character("a")){
      screen.WithRestoredIO([&]{try{const auto name=ask("Source ID"),url=ask("GitHub / Hugging Face repo or HTTPS manifest URL");client_action(o,{{"action","source.add"},{"name",name},{"url",url}});message="Source added";}catch(const std::exception& ex){message=ex.what();}})();
      data=inventory(o);update();return true;
    }
    if(rows.empty()||tab==2)return false;const auto id=rows[index];
    if(e!=Event::Character("i")&&e!=Event::Character("c")&&e!=Event::Character("o")&&e!=Event::Character("s")&&e!=Event::Character("x")&&e!=Event::Character("d"))return false;
    screen.WithRestoredIO([&]{try{
      if(e==Event::Character("i")){
        app_command({"show",id},o.root,o.config,binary(o));
        if(ask("Trust this publisher and install executable code on this device? [yes/no]")=="yes"){
          client_action(o,{{"action","app.install"},{"id",id},{"accept_code",true}});wizard(o,id);
        }
      } else if(e==Event::Character("c"))wizard(o,id);
      else if(e==Event::Character("o"))client_action(o,{{"action","app.open"},{"id",id},{"accept_code",true}});
      else if(e==Event::Character("s")){if(ask("Launch trusted app in background? [yes/no]")=="yes")client_action(o,{{"action","app.start"},{"id",id},{"accept_code",true}});}
      else if(e==Event::Character("x"))client_action(o,{{"action","app.stop"},{"id",id}});
      else if(ask("Unregister app? Files are retained for recovery [yes/no]")=="yes")client_action(o,{{"action","app.uninstall"},{"id",id}});
      message="Done";
    }catch(const std::exception& ex){message=ex.what();}})();data=inventory(o);update();return true;
  });screen.Loop(component);return 0;
}
int web(const ClientOptions& o,bool open) {
  // Loopback-only device manager. No CORS, no remote shell, separate management token.
  const auto secret=o.root/"secrets/client-ui-key";
  if(!fs::exists(secret))write_private_file(secret,generate_api_key()+"\n");
  auto key=text_file(secret);while(!key.empty()&&(key.back()=='\n'||key.back()=='\r'))key.pop_back();
  const auto address="http://127.0.0.1:"+std::to_string(o.port);
  httplib::Server server;server.set_payload_max_length(64*1024*1024);server.new_task_queue=[] {return new httplib::ThreadPool(4);};
  std::mutex operation;
  std::mutex sessions_mutex;
  std::map<std::string,std::pair<std::string,std::chrono::steady_clock::time_point>> sessions;
  server.set_pre_routing_handler([&](const httplib::Request& req,httplib::Response& res){
    const auto host=req.get_header_value("Host"),from=req.get_header_value("Origin");
    const auto localhost="http://localhost:"+std::to_string(o.port);
    if((host!="127.0.0.1:"+std::to_string(o.port)&&host!="localhost:"+std::to_string(o.port))||
       (!from.empty()&&from!=address&&from!=localhost)){
      res.status=403;res.set_content("Device-local access only","text/plain");return httplib::Server::HandlerResponse::Handled;
    }
    res.set_header("Cache-Control","no-store");res.set_header("X-Content-Type-Options","nosniff");
    if(!req.path.starts_with("/apps/"))res.set_header("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'");
    if(req.path.starts_with("/api/")&&req.get_header_value("Authorization")!="Bearer "+key){res.status=401;res.set_content("{\"error\":\"Client management key required\"}","application/json");return httplib::Server::HandlerResponse::Handled;}
    return httplib::Server::HandlerResponse::Unhandled;
  });
  auto assets=o.config.parent_path()/"apps/client-ui";
  for(const auto& [route,file,type]:std::vector<std::tuple<std::string,std::string,std::string>>{
      {"/","index.html","text/html"},{"/client.js","client.js","text/javascript"},{"/client.css","client.css","text/css"}})
    server.Get(route,[=](const httplib::Request&,httplib::Response& res){res.set_content(text_file(assets/file),type);});
  server.Get("/health",[](const httplib::Request&,httplib::Response& res){res.set_content("{\"service\":\"mica-client\",\"status\":\"ok\"}","application/json");});
  server.Get("/api/inventory",[&](const httplib::Request&,httplib::Response& res){std::lock_guard lock(operation);res.set_content(inventory(o).dump(),"application/json");});
  server.Post("/api/action",[&](const httplib::Request& req,httplib::Response& res){
    try {
      const auto request=json::parse(req.body);const auto action=request.at("action").get<std::string>();
      if(req.body.size()>65536)throw std::invalid_argument("Management request exceeds 64 KiB");
      // A browser cannot get a usable native terminal or silently install infrastructure.
      if(action=="app.open")throw std::invalid_argument("Use the displayed mica-client terminal command for native UI");
      std::lock_guard lock(operation);
      if(action=="web.open"){
        const auto id=request.at("id").get<std::string>();auto doc=app_manifest(o,id);
        if(!doc.at("launch").contains("port")&&!doc.at("launch").contains("static_entry"))throw std::invalid_argument("Native app: open it from mica-client tui on this device");
        auto apps=app_inventory(o.root,o.config);bool running=false;
        for(const auto& a:apps.at("apps"))if(plain_id(a.at("id"))==plain_id(id))running=a.value("running",false);
        if(!running)client_action(o,{{"action","app.start"},{"id",id},{"accept_code",request.value("accept_code",false)}});
        const auto token=generate_api_key(),prefix="/apps/"+plain_id(id);
        {std::lock_guard session_lock(sessions_mutex);
          const auto now=std::chrono::steady_clock::now();std::erase_if(sessions,[&](const auto& item){return item.second.second<now;});
          if(sessions.size()>=256)throw std::runtime_error("Too many active app sessions; restart client UI to clear");
          sessions[token]={plain_id(id),now+std::chrono::hours(8)};
        }
        res.set_header("Set-Cookie","mica_client_app="+token+"; Path="+prefix+"/; HttpOnly; SameSite=Strict; Max-Age=28800");
        res.set_content(json{{"url",prefix+(doc.at("launch").contains("static_entry")?std::string("/"):doc.at("launch").at("path").get<std::string>())}}.dump(),"application/json");
      }else res.set_content(client_action(o,request).dump(),"application/json");
    }catch(const std::exception& e){res.status=400;res.set_content(json{{"error",e.what()}}.dump(),"application/json");}
  });
  const auto proxy=[&](const httplib::Request& req,httplib::Response& res){
    const auto id=req.matches[1].str();bool authenticated=false;const auto cookie=req.get_header_value("Cookie");
    {std::lock_guard lock(sessions_mutex);for(const auto& [token,session]:sessions)if(session.first==id&&session.second>std::chrono::steady_clock::now()){
      std::istringstream parts(cookie);std::string part;while(std::getline(parts,part,';')){part.erase(0,part.find_first_not_of(' '));if(part=="mica_client_app="+token)authenticated=true;}
    }}
    if(!authenticated){res.status=401;res.set_content("Open this app from Mica Client first","text/plain");return;}
    try{serve_app_http(req,res,o.root,id);}catch(const std::exception& e){res.status=502;res.set_content(json{{"error",e.what()}}.dump(),"application/json");}
  };
  server.Get(R"(/apps/([A-Za-z0-9_-]+)(/.*))",proxy);
  server.Post(R"(/apps/([A-Za-z0-9_-]+)(/.*))",proxy);
  server.Put(R"(/apps/([A-Za-z0-9_-]+)(/.*))",proxy);
  server.Delete(R"(/apps/([A-Za-z0-9_-]+)(/.*))",proxy);
  server.Post("/api/logout",[&](const httplib::Request&,httplib::Response& res){std::lock_guard lock(sessions_mutex);sessions.clear();res.set_content("{\"status\":\"disconnected\"}","application/json");});
  server.set_exception_handler([](const httplib::Request&,httplib::Response& res,std::exception_ptr){res.status=500;res.set_content("{\"error\":\"Client operation failed\"}","application/json");});
  if(!server.bind_to_port("127.0.0.1",o.port))throw std::runtime_error("Client UI port busy; refusing to replace another service");
  std::cout<<"Mica Client: "<<address<<"/\nClient management key file: "<<secret<<"\nThis UI manages this device, not a remote server. Ctrl-C stops it.\n"<<std::flush;
  if(open){try{open_browser(address+"/");}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}}
  return server.listen_after_bind()?0:1;
}
void usage() {
  std::cout<<R"(Mica Client — install apps on THIS device; inference can be local or remote.
No local inference server, engine or model is required for remote connections.

  mica-client detect                      Detect local server binary/config/live endpoint
  mica-client list                        Installed apps on this device
  mica-client discover                    Available apps from shared publisher catalogues
  mica-client show APP_ID                  Inspect manifest, permissions and workload hints
  mica-client install APP_ID --accept-code [--inference local|remote|later]
    Remote: add/select --connection NAME; local setup requires separate confirmations.
  mica-client source add SOURCE_ID REPO_OR_MANIFEST_URL
  mica-client source remove SOURCE_ID
  mica-client connections                 Saved servers (keys redacted)
  mica-client connect NAME --url http://HOST:55000 --workload ID --api-key-file PATH
  mica-client bind APP_ID --connection NAME
  mica-client setup-local APP_ID --workload APP_ID--mac --accept-downloads [--install-server]
  mica-client open APP_ID --accept-code    Browser UI or declared foreground terminal UI
  mica-client start APP_ID --accept-code   Background app, independent of server lifecycle
  mica-client stop APP_ID
  mica-client uninstall APP_ID             Unregister; original files retained
  mica-client upgrade APP_ID --accept-code
  mica-client tui                         Native C++/Lua terminal interface
  mica-client ui [--open]                  Loopback HTML manager, http://127.0.0.1:55001/
  mica-client --version

Options: --root PATH (default ~/.mica/client), --server-root PATH (default ~/.mica),
         --config-dir PATH, --connection NAME, --port 55XXX (client UI).
Dashboard/API: 55000 (/dashboard shares API port). Client UI: 55001.
Private app services: 55200–55399. Inference workers: 55900–55999.
Existing explicitly configured server ports are preserved; no network scanning.
Install only registers verified app files/workloads, never silently starts a server.
Local setup may download many GB; review hardware/memory with mica-server plan first.
Browser UI cannot launch native windows/terminals: use its displayed local command.
)";
}
}

json client_detect(const ClientOptions& o) {
  const auto program=binary(o);
  const auto settings=load_server_configuration(o.server_root/"config/server.json");
  auto state=local_server_status(o.server_root);
  auto host=settings.value("host",std::string("127.0.0.1"));if(host=="0.0.0.0"||host=="::")host="127.0.0.1";
  auto url="http://"+(host.find(':')!=std::string::npos?"["+host+"]":host)+":"+std::to_string(settings.value("port",ports::server));
  // /health is public. Probe exactly the configured local listener, not other LAN devices.
  bool healthy=false;std::string revision;
  if(host=="127.0.0.1"||host=="localhost"||host=="::1"){
    try {httplib::Client probe(host,settings.value("port",ports::server));probe.set_connection_timeout(0,300000);probe.set_read_timeout(0,300000);
      auto r=probe.Get("/health");if(r&&r->status==200){auto h=json::parse(r->body);healthy=h.value("status","")=="ok"&&h.contains("version");revision=h.value("revision","");}}catch(...){}
  }
  return {{"installed",!program.empty()},{"binary",program.string()},{"running",healthy},
    {"status",state.value("status",healthy?"running":"stopped")},{"url",url},{"revision",revision},{"server_root",o.server_root.string()}};
}

json client_action(const ClientOptions& o,const json& request) {
  const auto action=request.at("action").get<std::string>();
  if(action=="detect")return client_detect(o);
  if(action=="inventory")return inventory(o);
  if(action=="app.show"){
    auto command=std::vector<std::string>{o.executable.string(),"show",request.at("id"),"--root",o.root.string(),"--server-root",o.server_root.string(),"--config-dir",o.config.string()};
    const auto result=run_command(command,true);if(result.exit_code)throw std::runtime_error(result.output);
    auto doc=json::parse(result.output);doc["local_setup_requirements"]=json::object();
    for(const auto& workload:doc.value("registered_workloads",json::array())){
      auto definition=read_profile_file(o.root/"config/profiles"/(workload.get<std::string>()+".yaml"));
      doc["local_setup_requirements"][workload.get<std::string>()]=definition.value("memory",json::object());
    }
    return doc;
  }
  if(action=="connection.add"){
    const auto name=request.at("name").get<std::string>();if(!id_ok(name))throw std::invalid_argument("Invalid connection name");
    auto address=origin(request.at("url"));auto workload=request.at("workload").get<std::string>();
    if(!std::regex_match(workload,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))throw std::invalid_argument("Invalid workload ID");
    auto path=o.root/"secrets/connections"/name;
    auto value=request.contains("api_key")?request.at("api_key").get<std::string>():text_file(request.at("api_key_file").get<std::string>());
    while(!value.empty()&&(value.back()=='\n'||value.back()=='\r'))value.pop_back();
    if(value.size()<16||value.size()>512||std::any_of(value.begin(),value.end(),[](unsigned char c){return c<32||c>126;}))throw std::invalid_argument("Invalid inference key");
    // Never replace a working connection until URL/key are authenticated.
    const auto pending=o.root/"secrets"/("pending-"+generate_api_key().substr(0,16));
    write_private_file(pending,value+"\n");
    struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove(p,ec);}}cleanup{pending};
    auto h=remote_get(o,address,"/health");if(h.value("status","")!="ok"||!h.contains("version"))throw std::runtime_error("Not a healthy Mica server");
    auto models=remote_get(o,address,"/v1/models?workload="+workload,pending);if(!models.contains("data")||!models.at("data").is_array())throw std::runtime_error("Invalid models response");
    write_private_file(path,value+"\n");auto saved=read(o.root/"config/connections.json");
    saved[name]={{"url",address},{"workload",workload},{"api_key_file",path.string()}};
    write_private_file(o.root/"config/connections.json",saved.dump(2));return {{"status","saved"},{"connection",name},{"url",address}};
  }
  if(action=="app.bind"){bind_app_connection(o,request.at("id"),request.at("name"));return {{"status","connected"}};}
  if(action=="server.setup-local")return local_setup(o,request);
  std::vector<std::string> args;
  if(action=="source.add")args={"source-add",request.at("name"),request.at("url")};
  else if(action=="source.remove")args={"source-remove",request.at("name")};
  else {
    static const std::set<std::string> allowed{"app.install","app.upgrade","app.start","app.open","app.stop","app.uninstall","app.show"};
    if(!allowed.contains(action))throw std::invalid_argument("Unknown client action");
    const auto id=request.at("id").get<std::string>();if(!id_ok(plain_id(id)))throw std::invalid_argument("Invalid app ID");
    args={action.substr(4),id};
    if(request.value("accept_code",false))args.push_back("--accept-code");
    if(request.contains("connection"))args.insert(args.end(),{"--connection",request.at("connection")});
    if(action=="app.start"||action=="app.open"){
      const auto manifest=app_manifest(o,id);
      const bool needs_connection=!manifest.at("requirements").at("operations").empty();
      auto connections=read(o.root/"config/app-connections"/(plain_id(id)+".json"));
      const auto name=request.value("connection",connections.value("default",std::string()));
      if(needs_connection||!name.empty()){
        if(name.empty())throw std::runtime_error("Configure this app's local/remote server connection first: mica-client bind APP --connection NAME");
        const auto saved=read(o.root/"config/connections.json");if(!saved.contains(name))throw std::runtime_error("Unknown server connection");
        compatible_connection(o,id,saved.at(name));
      }
      if(action=="app.open"&&(manifest.at("launch").contains("port")||manifest.at("launch").contains("static_entry"))){
        const auto url="http://127.0.0.1:"+std::to_string(o.port)+"/?app="+plain_id(id);
        httplib::Client probe("127.0.0.1",o.port);probe.set_connection_timeout(0,300000);probe.set_read_timeout(0,300000);auto response=probe.Get("/health");
        if(!response||response->status!=200||json::parse(response->body).value("service","")!="mica-client")
          throw std::runtime_error("Start the local manager first: mica-client ui --open. Then open this app from Installed.");
        std::cout<<url<<'\n';open_browser(url);return {{"status","opened"},{"url",url}};
      }
    }
  }
  const auto status=app_command(args,o.root,o.config,binary(o));if(status)throw std::runtime_error("App command failed");
  return {{"status","ok"}};
}

int client_main(int argc,char** argv) {
  ClientOptions o;const auto home=std::getenv("HOME");if(!home)throw std::runtime_error("HOME missing");
  const auto mica_home=std::getenv("MICA_HOME");
  o.server_root=mica_home&&*mica_home?fs::path(mica_home):fs::path(home)/".mica";o.root=o.server_root/"client";
  auto input=fs::path(argv[0]);
  if(input.has_parent_path())o.executable=fs::weakly_canonical(fs::absolute(input));
  else {const auto raw=std::getenv("PATH");std::istringstream paths(raw?raw:"");std::string part;while(std::getline(paths,part,':')){auto p=fs::path(part)/input;if(access(p.c_str(),X_OK)==0){o.executable=fs::weakly_canonical(p);break;}}}
  std::vector<std::string> args;json req;bool accept=false,downloads=false,install=false,open=false;std::string connection,mode;
  if(argc==1){usage();return 0;}
  for(int i=1;i<argc;++i){std::string a=argv[i];auto next=[&]{if(++i>=argc)throw std::invalid_argument("Missing option value");return std::string(argv[i]);};
    if(a=="--help"||a=="-h"){usage();return 0;}
    if(a=="--version"){std::cout<<"mica-client "<<MICA_SERVER_VERSION<<" ("<<MICA_SERVER_REVISION<<")\n";return 0;}
    if(a=="--root")o.root=next();else if(a=="--server-root")o.server_root=next();else if(a=="--config-dir")o.config=next();
    else if(a=="--port")o.port=std::stoi(next());else if(a=="--url")req["url"]=next();else if(a=="--workload")req["workload"]=next();
    else if(a=="--api-key-file")req["api_key_file"]=next();else if(a=="--connection")connection=next();else if(a=="--inference")mode=next();
    else if(a=="--accept-code")accept=true;else if(a=="--accept-downloads")downloads=true;else if(a=="--install-server")install=true;else if(a=="--open")open=true;
    else if(a.starts_with('-'))throw std::invalid_argument("Unknown option: "+a);else args.push_back(a);
  }
  if(args.empty())throw std::invalid_argument("Choose a command; see --help");
  if(!mode.empty()&&mode!="local"&&mode!="remote"&&mode!="later")throw std::invalid_argument("--inference must be local, remote or later");
  if(o.port<55000||o.port>55999)throw std::invalid_argument("Client UI port must be in 55000..55999");
  if(o.config.empty()){
    if(auto env=std::getenv("MICA_CONFIG_DIR");env&&*env)o.config=env;
    else for(const auto& candidate:std::vector<fs::path>{o.executable.parent_path().parent_path()/"share/mica-server/config",o.executable.parent_path().parent_path()/"config",fs::current_path()/"config"})
      if(fs::is_directory(candidate/"engines")){o.config=candidate;break;}
  }
  o.root=fs::absolute(o.root);o.server_root=fs::absolute(o.server_root);
  if(o.root==o.server_root)throw std::invalid_argument("Client root must be separate from server state root");
  if(args[0]=="detect"){std::cout<<client_detect(o).dump(2)<<'\n';return 0;}
  if(o.config.empty())throw std::runtime_error("Install the matching shared configuration files or use --config-dir");
  o.config=fs::absolute(o.config);
  if(args[0]=="ui")return web(o,open);
  if(args[0]=="tui")return tui(o);
  if(args[0]=="connections"){std::cout<<connection_view(o).dump(2)<<'\n';return 0;}
  if(args[0]=="list"||args[0]=="discover"){
    auto data=app_inventory(o.root,o.config);if(args[0]=="list"){
      auto filtered=json::array();for(const auto& a:data["apps"])if(a.value("installed",false))filtered.push_back(a);data["apps"]=filtered;
    }std::cout<<data.dump(2)<<'\n';return 0;
  }
  auto require=[&](std::size_t n){if(args.size()!=n)throw std::invalid_argument("Wrong arguments; see mica-client --help");};
  req["accept_code"]=accept;
  if(args[0]=="source"){
    if(args.size()<2)throw std::invalid_argument("source add/remove");require(args[1]=="add"?4:3);
    req["action"]=args[1]=="add"?"source.add":args[1]=="remove"?"source.remove":"invalid";req["name"]=args[2];if(args.size()==4)req["url"]=args[3];
  }else if(args[0]=="connect") {require(2);req["action"]="connection.add";req["name"]=args[1];if(!req.contains("api_key_file"))req["api_key"]=ask("Inference API key",true);}
  else {require(2);req["id"]=args[1];req["action"]="app."+args[0];if(!connection.empty())req["connection"]=connection;
    if(args[0]=="bind"){req["action"]="app.bind";req["name"]=connection;}
    if(args[0]=="setup-local"){req["action"]="server.setup-local";req["accept_downloads"]=downloads;req["install_server"]=install;}
  }
  // Avoid mixing a manifest's JSON with a second status JSON document.
  if(args[0]=="show")return app_command({"show",args.at(1)},o.root,o.config,binary(o));
  auto result=client_action(o,req);std::cout<<result.dump(2)<<'\n';
  if(args[0]=="install"){
    if(mode=="remote"){
      if(connection.empty())throw std::invalid_argument("App installed; select --connection NAME or run mica-client bind");bind_app_connection(o,args[1],connection);
    } else if(mode=="local"){
      req["accept_downloads"]=downloads;req["install_server"]=install;std::cout<<local_setup(o,req).dump(2)<<'\n';
    } else if(mode.empty()&&isatty(STDIN_FILENO))wizard(o,args[1]);
    else std::cout<<"App installed without inference setup. Use bind (existing server) or setup-local.\n";
  }
  return 0;
}
}

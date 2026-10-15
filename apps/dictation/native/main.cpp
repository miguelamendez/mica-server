#include "platform.hpp"
#include <nlohmann/json.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <regex>
#include <vector>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
extern char** environ;
using json=nlohmann::json;
namespace fs=std::filesystem;
static std::atomic<bool> stopping{false};
static json read(const fs::path& path){std::ifstream input(path);return input?json::parse(input):json::object();}
static void save(const fs::path& path,const json& value){fs::create_directories(path.parent_path());if(fs::is_symlink(path))throw std::runtime_error("refusing symlink configuration");auto pending=path.string()+".pending";int fd=open(pending.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)throw std::runtime_error("cannot save configuration");auto data=value.dump(2);std::size_t at=0;while(at<data.size()){auto n=write(fd,data.data()+at,data.size()-at);if(n<=0){close(fd);throw std::runtime_error("configuration write failed");}at+=n;}fsync(fd);close(fd);fs::rename(pending,path);}
struct Temporary {fs::path path;Temporary(){std::string pattern=(fs::temp_directory_path()/"mica-dictation-XXXXXX").string();auto result=mkdtemp(pattern.data());if(!result)throw std::runtime_error("cannot create private temporary directory");path=result;}~Temporary(){std::error_code e;fs::remove_all(path,e);}};
static std::string execute(std::vector<std::string> args){int pipes[2];if(pipe(pipes))throw std::runtime_error("pipe failed");for(auto fd:pipes)fcntl(fd,F_SETFD,FD_CLOEXEC);posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,pipes[1],1);posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);posix_spawn_file_actions_addclose(&actions,pipes[0]);std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);pid_t pid;auto code=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);posix_spawn_file_actions_destroy(&actions);close(pipes[1]);if(code){close(pipes[0]);throw std::runtime_error("required program unavailable (curl)");}std::string out;char buffer[8192];ssize_t n;while((n=::read(pipes[0],buffer,sizeof(buffer)))>0){out.append(buffer,n);if(out.size()>16*1024*1024){kill(pid,SIGTERM);break;}}close(pipes[0]);int status;waitpid(pid,&status,0);if(!WIFEXITED(status)||WEXITSTATUS(status))throw std::runtime_error("Mica request failed; check URL, inference key, workload and server availability");return out;}
struct Connection {
  std::string url,key_file,workload;
  json request(const std::string& route,const json* body=nullptr,const fs::path& audio={})const{
    if(!std::regex_match(url,std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/?)")))throw std::runtime_error("Mica URL must be an HTTP(S) origin without credentials/path");
    std::ifstream input(key_file);std::string key;std::getline(input,key);if(!key.empty()&&key.back()=='\r')key.pop_back();
    if(key.size()<16||key.size()>512||key.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("private inference key file missing or invalid");
    if(!std::regex_match(workload,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")))throw std::runtime_error("workload ID required");
    Temporary temporary;auto headers=temporary.path/"headers";{std::ofstream f(headers);f<<"Authorization: Bearer "<<key<<"\nX-Mica-Workload: "<<workload<<"\n";}fs::permissions(headers,fs::perms::owner_read|fs::perms::owner_write);
    auto base=url;while(base.ends_with('/'))base.pop_back();
    std::vector<std::string> command={"curl","--silent","--show-error","--fail-with-body","--proto","=http,https","--max-time","300","--header","@"+headers.string(),base+route};
    if(!audio.empty()){
      if(!fs::is_regular_file(audio)||fs::file_size(audio)>64*1024*1024)throw std::runtime_error("audio must be a file of at most 64 MiB");
      auto copy=temporary.path/"recording.wav";fs::copy_file(audio,copy);command.insert(command.end(),{"--form","file=@"+copy.string(),"--form","response_format=json"});
    }else if(body){auto payload=temporary.path/"payload.json";save(payload,*body);command.insert(command.end(),{"--header","Content-Type: application/json","--data-binary","@"+payload.string()});}
    return json::parse(execute(command));
  }
  std::string transcribe(const fs::path& audio,bool cleanup)const{
    auto result=request("/v1/audio/transcriptions",nullptr,audio);auto text=result.at("text").get<std::string>();
    if(cleanup&&!text.empty()){
      json body={{"stream",false},{"max_tokens",2048},{"messages",json::array({{{"role","system"},{"content","Clean up dictated text. Preserve meaning, language and details. Fix punctuation and obvious transcription errors only. Return only the corrected text."}},{{"role","user"},{"content",text}}})}};
      try{text=request("/v1/chat/completions",&body).at("choices").at(0).at("message").at("content");}
      catch(...){std::cerr<<"Cleanup unavailable; original transcript retained.\n";}
    }return text;
  }
};
int main(int argc,char** argv){
 try{
  const auto home=std::getenv("HOME");if(!home)throw std::runtime_error("HOME not set");fs::path root=fs::path(home)/".mica",config=root/"config/server.json",audio;
  std::string workload,connection_name;bool tui=false,cleanup=false,json_output=false,automatic_type=false;
  for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--tui")tui=true;else if(arg=="--cleanup")cleanup=true;else if(arg=="--json")json_output=true;else if(arg=="--auto-type")automatic_type=true;else if(arg=="--help"){std::cout<<"mica-dictation [--tui] [--connection NAME] [--workload ID] [--transcribe WAV] [--cleanup] [--auto-type]\nCtrl+Option/Alt+R toggles recording. --tui configures local/remote servers. MCP: python3 mcp.py\n";return 0;}else if(arg=="--config"||arg=="--root"||arg=="--workload"||arg=="--connection"||arg=="--transcribe"){if(++i==argc)throw std::runtime_error("missing option value");if(arg=="--config")config=argv[i];else if(arg=="--root")root=argv[i];else if(arg=="--workload")workload=argv[i];else if(arg=="--connection")connection_name=argv[i];else audio=argv[i];}else throw std::runtime_error("unknown option: "+arg);}
  const auto path=root/"config/app-connections/mica-dictation.json";auto saved=read(path);auto document=read(config);
  if(connection_name.empty())connection_name=saved.value("default","");Connection connection;
  if(!connection_name.empty()){auto selected=saved.at("connections").at(connection_name);connection={selected.at("url"),selected.at("api_key_file"),selected.at("workload")};}
  else{auto host=document.value("host","127.0.0.1");if(host=="0.0.0.0"||host=="::")host="127.0.0.1";connection={document.value("ui",json::object()).value("mica_url","http://"+host+":"+std::to_string(document.value("port",55000))),document.value("api_key_file",(root/"secrets/api-key").string()),workload.empty()?document.value("default_workload",""):workload};}
  if(!workload.empty())connection.workload=workload;
  if(!audio.empty()){const auto text=connection.transcribe(audio,cleanup);std::cout<<(json_output?json{{"text",text}}.dump():text)<<'\n';return 0;}
  using namespace ftxui;auto screen=ScreenInteractive::Fullscreen();std::mutex mutex;std::thread worker;std::atomic<bool> busy{false},hotkey{false},recording{false};std::atomic<long long> recording_start{0};bool shortcut_enabled=false;long focus=0;std::string status="Set a server connection, then record. Models run only on the selected server.",transcript;
  Temporary recording_file;auto wav=recording_file.path/"recording.wav";
  const auto toggle=[&]{if(busy.exchange(true))return;if(worker.joinable())worker.join();worker=std::thread([&]{try{std::string message;if(!recording){focus=focused_application();if(!microphone_start(wav.string(),message))throw std::runtime_error(message);recording_start=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();recording=true;message="Recording (maximum 2 minutes) — press Ctrl+Alt/Option+R again to stop.";}else{microphone_stop();recording=false;{std::lock_guard lock(mutex);status="Transcribing on "+connection.url;}screen.PostEvent(Event::Custom);auto text=connection.transcribe(wav,cleanup);deliver_text(text,automatic_type,focus,message);{std::lock_guard lock(mutex);transcript=std::move(text);}if(message.empty())message=automatic_type?"Transcript delivered.":"Transcript copied. Paste where needed.";fs::remove(wav);}std::lock_guard lock(mutex);status=message;}catch(const std::exception& error){microphone_stop();recording=false;std::lock_guard lock(mutex);status=error.what();}busy=false;screen.PostEvent(Event::Custom);});};
  std::string name=connection_name.empty()?"remote":connection_name;
  auto input_name=Input(&name,"Connection name"),input_url=Input(&connection.url,"http://HOST:PORT"),input_workload=Input(&connection.workload,"Workload ID"),input_key=Input(&connection.key_file,"Private INFERENCE key file path");
  const auto saved_connections=saved.value("connections",json::object());auto connection_labels=std::vector<std::string>{};for(const auto& [id,_]:saved_connections.items())connection_labels.push_back(id);int selected_connection=0;
  auto choose=Dropdown(&connection_labels,&selected_connection);
  auto select=Button("Use selected connection",[&]{if(busy||recording)return;try{if(connection_labels.empty())throw std::runtime_error("Save a connection first");name=connection_labels.at(selected_connection);auto item=read(path).at("connections").at(name);connection={item.at("url"),item.at("api_key_file"),item.at("workload")};saved["default"]=name;save(path,saved);std::lock_guard lock(mutex);status="Selected "+name;}catch(const std::exception& e){std::lock_guard lock(mutex);status=e.what();}});
  auto store=Button("Validate and save connection",[&]{if(busy||recording)return;busy=true;if(worker.joinable())worker.join();worker=std::thread([&]{try{if(!std::regex_match(name,std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,79}")))throw std::runtime_error("invalid connection name");auto models=connection.request("/v1/models");bool asr=false;for(auto& model:models.value("data",json::array()))for(auto& interaction:model.value("supported_interactions",json::array()))asr|=interaction.value("operation","")=="audio.transcribe";if(!asr)throw std::runtime_error("workload has no transcription model");saved["schema"]=1;saved["default"]=name;saved["connections"][name]={{"url",connection.url},{"workload",connection.workload},{"api_key_file",connection.key_file}};save(path,saved);std::lock_guard lock(mutex);status="Saved "+name+". No local models or engines installed.";}catch(const std::exception& e){std::lock_guard lock(mutex);status=e.what();}busy=false;screen.PostEvent(Event::Custom);});});
  auto cleanup_check=Checkbox("Clean up transcript with workload's LLM",&cleanup),type_check=Checkbox("Paste into original focused app (macOS Accessibility)",&automatic_type);
  auto record_button=Button("Record / stop",toggle);
  auto enable=Button("Enable global Ctrl+Alt/Option+R",[&]{if(shortcut_enabled)return;std::string message;shortcut_enabled=shortcut_start([&]{hotkey=true;screen.PostEvent(Event::Custom);},message);std::lock_guard lock(mutex);status=shortcut_enabled?"Global shortcut active. Recording begins only when you press it.":message;});
  auto quit=Button("Quit",screen.ExitLoopClosure());
  auto controls=Container::Vertical({choose,select,input_name,input_url,input_workload,input_key,store,cleanup_check,type_check,record_button,enable,quit});
  auto view=Renderer(controls,[&]{std::lock_guard lock(mutex);return vbox({text("Mica Dictation · speak → transcribe → optional cleanup → paste")|bold,separator(),text("App runs on this device. Inference uses the selected local/remote Mica server."),choose->Render(),select->Render(),hbox({text("Name      "),input_name->Render()}),hbox({text("Server    "),input_url->Render()}),hbox({text("Workload  "),input_workload->Render()}),hbox({text("Key file  "),input_key->Render()}),store->Render(),separator(),cleanup_check->Render(),type_check->Render(),hbox({record_button->Render(),enable->Render(),quit->Render()}),separator(),paragraph(status)|color(busy?Color::Yellow:Color::Green),paragraph(transcript)|flex})|border;});
  auto handler=CatchEvent(view,[&](Event event){if(hotkey.exchange(false)){toggle();return true;}if(busy&&event!=Event::Custom)return true;return false;});
  signal(SIGINT,[](int){stopping=true;});signal(SIGTERM,[](int){stopping=true;});
  const auto expired=[&]{return recording&&std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count()-recording_start>=120;};
  if(tui){std::atomic<bool> timer_stop{false};std::thread timer([&]{while(!timer_stop){if(expired()){hotkey=true;screen.PostEvent(Event::Custom);}if(stopping){screen.ExitLoopClosure()();break;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}});screen.Loop(handler);timer_stop=true;timer.join();}else{std::string error;if(!shortcut_start([&]{hotkey=true;},error))throw std::runtime_error(error+" Run mica-dictation --tui to configure/test recording.");shortcut_enabled=true;std::cerr<<"Dictation active: Ctrl+Alt/Option+R toggles recording. Ctrl+C exits.\n";while(!stopping){if(hotkey.exchange(false)||expired())toggle();std::this_thread::sleep_for(std::chrono::milliseconds(50));}}
  shortcut_stop();if(worker.joinable())worker.join();microphone_stop();return 0;
 }catch(const std::exception& error){std::cerr<<"Dictation: "<<error.what()<<'\n';return 1;}
}

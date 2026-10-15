#include "mica_server/app_http.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/apps.hpp"
#include "mica_server/setup.hpp"
#include <nlohmann/json.hpp>
#include <deque>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <mutex>
#include <regex>
#include <algorithm>
namespace mica {
namespace {
std::string contents(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("App file missing");return {std::istreambuf_iterator<char>(f),{}};}
std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});return s;}
std::string html_prefix(std::string html,const std::string& prefix){
  for(const auto* attr:{"src=\"/","href=\"/"}){std::size_t p=0;const std::string needle=attr;
    while((p=html.find(needle,p))!=std::string::npos){const auto start=p+needle.size()-1;if(start+1<html.size()&&html[start+1]!='/'){html.insert(start,prefix);p=start+prefix.size()+1;}else ++p;}}
  // The demo uses root-relative fetches; keep them beneath the app prefix.
  html.insert(0,"<script>const _micaFetch=window.fetch.bind(window);window.fetch=(u,o)=>_micaFetch(typeof u==='string'&&u.startsWith('/')&&!u.startsWith('//')?"+nlohmann::json(prefix).dump()+"+u:u,o);</script>");return html;
}
struct Bridge {
  httplib::Client client;std::mutex mutex;std::condition_variable changed;std::deque<std::string> chunks;
  std::thread reader;httplib::Headers headers;std::size_t queued{0};int status{502};bool ready{false},finished{false},cancelled{false};
  explicit Bridge(int port):client("127.0.0.1",port){client.set_connection_timeout(3,0);client.set_read_timeout(3600,0);}
  ~Bridge(){{std::lock_guard lock(mutex);cancelled=true;}changed.notify_all();client.stop();if(reader.joinable())reader.join();}
  void start(httplib::Request request){request.response_handler=[this](const auto& response){std::lock_guard lock(mutex);status=response.status;headers=response.headers;ready=true;changed.notify_all();return !cancelled;};
    request.content_receiver=[this](const char* data,std::size_t size,std::size_t,std::size_t){std::unique_lock lock(mutex);changed.wait(lock,[&]{return cancelled||queued<262144;});if(cancelled)return false;chunks.emplace_back(data,size);queued+=size;changed.notify_all();return true;};
    reader=std::thread([this,request=std::move(request)]{try{client.send(request);}catch(...){} {std::lock_guard lock(mutex);ready=finished=true;}changed.notify_all();});
  }
};
}
void serve_app_http(const httplib::Request& request,httplib::Response& response,const std::filesystem::path& root,const std::string& id){
  namespace fs=std::filesystem;using json=nlohmann::json;
  if(!std::regex_match(id,std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,79}")))throw std::invalid_argument("Invalid app ID");
  const auto base=root/"apps"/id;const auto doc=read_profile_file(base/"app.yaml");validate_app_manifest(doc);
  const auto prefix="/apps/"+id;auto suffix=request.path.substr(prefix.size());if(suffix.empty())suffix="/";
  if(suffix.find("..")!=std::string::npos||suffix.find('\\')!=std::string::npos)throw std::invalid_argument("Unsafe app path");
  response.set_header("Cache-Control","no-store");response.set_header("X-Content-Type-Options","nosniff");response.set_header("Referrer-Policy","no-referrer");
  response.set_header("Content-Security-Policy","frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
  if(doc.at("launch").contains("static_entry")){
    if(request.method!="GET"){response.status=405;return;}
    const auto relative=suffix=="/"?doc.at("launch").at("static_entry").get<std::string>():suffix.substr(1);
    // Never serve app.yaml, receipts, connection configs or arbitrary app-local files.
    const auto& files=doc.at("distribution").at("files");
    if(std::none_of(files.begin(),files.end(),[&](const auto& f){return f.at("path")==relative;})){response.status=404;return;}
    auto path=base;for(const auto& part:fs::path(relative)){path/=part;if(fs::is_symlink(path))throw std::runtime_error("App symlink rejected");}
    auto body=contents(path);std::string mime="application/octet-stream";const auto extension=path.extension().string();
    if(extension==".html"){mime="text/html; charset=utf-8";body=html_prefix(body,prefix);}else if(extension==".js")mime="text/javascript";else if(extension==".css")mime="text/css";else if(extension==".svg")mime="image/svg+xml";else if(extension==".png")mime="image/png";
    response.set_content(body,mime);return;
  }
  auto state=json::parse(contents(root/"state/apps"/(id+".json")));std::smatch match;const auto url=state.at("url").get<std::string>();
  if(!std::regex_match(url,match,std::regex(R"(http://127\.0\.0\.1:([0-9]{1,5})(/.*))")))throw std::runtime_error("App is not a private loopback service");
  httplib::Request upstream;upstream.method=request.method;upstream.path=suffix;upstream.body=request.body;
  if(auto query=request.target.find('?');query!=std::string::npos)upstream.path+=request.target.substr(query);
  for(const auto& [key,value]:request.headers){const auto name=lower(key);if(name=="accept"||name=="content-type"||name=="range")upstream.headers.emplace(key,value);}
  const auto port=std::stoi(match[1]);upstream.headers.emplace("Origin","http://127.0.0.1:"+std::to_string(port));
  if(request.is_multipart_form_data()){
    const auto boundary="mica-"+generate_api_key();upstream.headers.erase("Content-Type");upstream.headers.emplace("Content-Type","multipart/form-data; boundary="+boundary);upstream.body.clear();
    auto quote=[](std::string value){for(auto& c:value)if(c=='\r'||c=='\n'||c=='"')c='_';return value;};
    for(const auto& [name,field]:request.form.fields)upstream.body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+quote(name)+"\"\r\n\r\n"+field.content+"\r\n";
    for(const auto& [name,file]:request.form.files)upstream.body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+quote(name)+"\"; filename=\""+quote(file.filename)+"\"\r\nContent-Type: application/octet-stream\r\n\r\n"+file.content+"\r\n";
    upstream.body+="--"+boundary+"--\r\n";
  }
  if(upstream.body.size()>64*1024*1024)throw std::runtime_error("App upload exceeds 64 MiB");
  auto bridge=std::make_shared<Bridge>(port);bridge->start(std::move(upstream));
  {std::unique_lock lock(bridge->mutex);if(!bridge->changed.wait_for(lock,std::chrono::seconds(10),[&]{return bridge->ready;}))throw std::runtime_error("App not ready; retry after startup");
    response.status=bridge->status;
    for(const auto& [key,value]:bridge->headers){const auto name=lower(key);if(name=="content-type"||name=="content-disposition"||name=="content-range"||name=="accept-ranges")response.set_header(key,value);if(name=="location"&&value.starts_with('/')&&!value.starts_with("//"))response.set_header("Location",prefix+value);}
  }
  const auto mime=response.get_header_value("Content-Type");
  if(mime.starts_with("text/html")){
    std::string body;std::unique_lock lock(bridge->mutex);for(;;){bridge->changed.wait(lock,[&]{return bridge->finished||!bridge->chunks.empty();});
      while(!bridge->chunks.empty()){body+=bridge->chunks.front();bridge->queued-=bridge->chunks.front().size();bridge->chunks.pop_front();bridge->changed.notify_all();}
      if(body.size()>4*1024*1024)throw std::runtime_error("App HTML exceeds 4 MiB");
      if(bridge->finished)break;
    }lock.unlock();response.set_content(html_prefix(body,prefix),mime);
  }else response.set_chunked_content_provider(mime.empty()?"application/octet-stream":mime,[bridge](std::size_t,httplib::DataSink& sink){
    std::unique_lock lock(bridge->mutex);bridge->changed.wait(lock,[&]{return bridge->finished||!bridge->chunks.empty();});
    if(bridge->chunks.empty()){sink.done();return true;}auto part=std::move(bridge->chunks.front());bridge->chunks.pop_front();bridge->queued-=part.size();bridge->changed.notify_all();lock.unlock();return sink.write(part.data(),part.size());
  });
}
}

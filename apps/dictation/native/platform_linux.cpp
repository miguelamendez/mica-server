#include "platform.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#ifdef MICA_DICTATION_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif
extern char** environ;
static pid_t recorder=-1;
static std::thread shortcut_thread;
static std::atomic<bool> quit_shortcut{false};
static bool exists(const char* command){const auto path=std::getenv("PATH");if(!path)return false;std::string all=path;std::size_t pos=0;while(pos<=all.size()){auto end=all.find(':',pos);if(access((all.substr(pos,end-pos)+"/"+command).c_str(),X_OK)==0)return true;if(end==std::string::npos)break;pos=end+1;}return false;}
static bool pipe_command(const char* program,const std::string& text){int pipes[2];if(pipe(pipes))return false;posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,pipes[0],0);posix_spawn_file_actions_addclose(&actions,pipes[1]);posix_spawn_file_actions_addclose(&actions,pipes[0]);char* args[]={const_cast<char*>(program),const_cast<char*>("-selection"),const_cast<char*>("clipboard"),nullptr};if(std::string(program)!="xclip")args[1]=nullptr;pid_t pid;const auto result=posix_spawnp(&pid,program,&actions,nullptr,args,environ);posix_spawn_file_actions_destroy(&actions);close(pipes[0]);if(result){close(pipes[1]);return false;}std::size_t n=0;while(n<text.size()){auto wrote=write(pipes[1],text.data()+n,text.size()-n);if(wrote<=0)break;n+=wrote;}close(pipes[1]);int status;waitpid(pid,&status,0);return WIFEXITED(status)&&WEXITSTATUS(status)==0;}
bool microphone_start(const std::string& path,std::string& error){
  std::vector<std::string> args;
  if(exists("pw-record"))args={"pw-record","--rate","16000","--channels","1","--format","s16",path};
  else if(exists("arecord"))args={"arecord","-q","-f","S16_LE","-r","16000","-c","1","-t","wav",path};
  else{error="Install PipeWire pw-record or ALSA arecord to record on this device.";return false;}
  std::vector<char*> native;for(auto& item:args)native.push_back(item.data());native.push_back(nullptr);
  posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
  const auto code=posix_spawnp(&recorder,native[0],&actions,nullptr,native.data(),environ);posix_spawn_file_actions_destroy(&actions);
  if(code){error="Cannot launch microphone recorder";return false;}usleep(100000);int status;if(waitpid(recorder,&status,WNOHANG)==recorder){recorder=-1;error="Microphone recorder failed. Check audio device/session permissions.";return false;}return true;
}
void microphone_stop(){if(recorder>0){kill(recorder,SIGINT);waitpid(recorder,nullptr,0);recorder=-1;}}
bool shortcut_start(std::function<void()> callback,std::string& error){
#ifdef MICA_DICTATION_X11
  if(std::getenv("WAYLAND_DISPLAY")){error="Wayland global shortcuts require a desktop portal; use the TUI Record button for now.";return false;}
  XInitThreads();auto display=XOpenDisplay(nullptr);if(!display){error="No desktop X11 session. Use the TUI Record button.";return false;}const auto key=XKeysymToKeycode(display,XK_r);const auto root=DefaultRootWindow(display);
  XSetErrorHandler([](Display*,XErrorEvent*){return 0;});
  for(unsigned int locks:std::vector<unsigned>{0U,LockMask,Mod2Mask,LockMask|Mod2Mask})XGrabKey(display,key,ControlMask|Mod1Mask|locks,root,False,GrabModeAsync,GrabModeAsync);XSync(display,False);quit_shortcut=false;
  shortcut_thread=std::thread([display,key,root,callback=std::move(callback)]{while(!quit_shortcut){if(XPending(display)){XEvent event;XNextEvent(display,&event);if(event.type==KeyPress&&event.xkey.keycode==key)callback();}else usleep(20000);}XUngrabKey(display,key,AnyModifier,root);XCloseDisplay(display);});return true;
#else
  error="This build has no X11 global-shortcut support. Use the TUI Record button.";return false;
#endif
}
void shortcut_stop(){quit_shortcut=true;if(shortcut_thread.joinable())shortcut_thread.join();}
long focused_application(){return 0;}
bool deliver_text(const std::string& text,bool type,long,std::string& error){
  const char* program=exists("wl-copy")?"wl-copy":exists("xclip")?"xclip":nullptr;
  if(!program){error="Transcript ready. Install wl-clipboard or xclip for clipboard support.";return false;}
  if(!pipe_command(program,text)){error="Clipboard unavailable in this desktop session.";return false;}
  if(type)error="Transcript copied. Automatic typing is currently macOS-only; paste manually on Linux.";
  return true;
}

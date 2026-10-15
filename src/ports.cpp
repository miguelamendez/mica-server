#include "mica_server/ports.hpp"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

namespace mica::ports {
Lease::~Lease() { close(descriptor_); }
void Lease::inherit_by_child() const {
  // Called only in the freshly forked app child. All other leases stay CLOEXEC.
  if(fcntl(descriptor_,F_SETFD,0)<0)_exit(126);
}
std::shared_ptr<Lease> acquire(bool app_service) {
  const auto directory=std::filesystem::temp_directory_path()/std::string("mica-ports-"+std::to_string(getuid()));
  if(mkdir(directory.c_str(),0700)!=0 && errno!=EEXIST)throw std::runtime_error("Cannot create machine port-lease directory");
  struct stat st{};
  if(lstat(directory.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077))
    throw std::runtime_error("Unsafe machine port-lease directory");
  const int first=app_service?app_first:worker_first,last=app_service?app_last:worker_last;
  for(int port=first;port<=last;++port){
    const auto path=directory/(std::to_string(port)+".lock");
    const int fd=open(path.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)continue;
    if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);continue;}
    const int sock=socket(AF_INET,SOCK_STREAM,0);
    if(sock<0){close(fd);throw std::runtime_error("Cannot probe service port");}
    // Match normal HTTP listeners: a dead service's TIME_WAIT connections must
    // not exhaust a reusable pool, but an active listener still makes bind fail.
    const int reuse=1;setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    const bool available=bind(sock,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;
    close(sock);
    if(available)return std::make_shared<Lease>(fd,port);
    close(fd);
  }
  throw std::runtime_error(std::string(app_service?"App service":"Inference worker")+" port pool exhausted ("+std::to_string(first)+"–"+std::to_string(last)+"); stop an unused service and retry");
}
}

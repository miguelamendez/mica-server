#include "mica_server/ports.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <vector>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
int main(){
  using namespace mica::ports;
  auto worker=acquire(false);const auto port=worker->port();
  assert(port>=worker_first&&port<=worker_last);
  auto other=acquire(false);assert(other->port()!=port);
  const int occupied=socket(AF_INET,SOCK_STREAM,0);assert(occupied>=0);
  const int reuse=1;assert(setsockopt(occupied,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))==0);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
  assert(bind(occupied,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);assert(listen(occupied,1)==0);
  worker.reset();auto skipped=acquire(false);assert(skipped->port()!=port);skipped.reset();close(occupied);
  worker=acquire(false);assert(worker->port()==port);
  // Another process must not allocate a slot held by this one.
  const auto child=fork();assert(child>=0);
  if(child==0){auto candidate=acquire(false);_exit(candidate->port()==port?1:0);}
  int status=0;waitpid(child,&status,0);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  worker.reset();auto reused=acquire(false);assert(reused->port()==port);other.reset();reused.reset();
  for(const bool apps:{false,true}){
    std::vector<std::shared_ptr<Lease>> leases;std::set<int> ports;
    // Existing live services may occupy slots; allocate until the bounded pool rejects.
    for(;;){try{auto lease=acquire(apps);assert(ports.insert(lease->port()).second);leases.push_back(lease);}catch(const std::runtime_error& e){assert(std::string(e.what()).find("pool exhausted")!=std::string::npos);break;}}
    assert(!leases.empty());assert(leases.size()<=static_cast<std::size_t>(apps?200:100));
    const auto released=leases.back()->port();leases.pop_back();auto again=acquire(apps);assert(again->port()==released);
  }
  std::cout<<"Port pools: range, cross-process exclusion, exhaustion and reuse passed\n";
}

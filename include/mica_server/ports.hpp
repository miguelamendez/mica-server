#pragma once
#include <memory>

namespace mica::ports {
// Mica defaults live in one documented, non-exclusive application range.
// These numbers are conventions, not an OS/IANA reservation. Never steal a port.
inline constexpr int server = 55000;       // API and optional /dashboard
inline constexpr int client = 55001;       // device-local client manager
inline constexpr int app_first = 55200;
inline constexpr int app_last = 55399;
inline constexpr int worker_first = 55900;
inline constexpr int worker_last = 55999;

// A machine-wide lease; close releases the cooperating-process lock. The TCP
// listener is owned by the service, not by this allocation object.
class Lease {
 public:
  Lease(int descriptor, int port) : descriptor_(descriptor), port_(port) {}
  ~Lease();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  int port() const { return port_; }
  void inherit_by_child() const;
 private:
  int descriptor_, port_;
};
std::shared_ptr<Lease> acquire(bool app_service);
}

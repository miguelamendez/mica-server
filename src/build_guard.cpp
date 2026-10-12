#include "mica_server/build_guard.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
extern char** environ;
namespace mica {
namespace {
volatile sig_atomic_t guarded_pid = 0, interrupted = 0;
void forward_build_signal(int signal) {
  interrupted = signal;
  if (guarded_pid > 0) kill(-guarded_pid, signal);
}
}
MemorySnapshot live_memory_snapshot() {
  MemorySnapshot result;
#ifdef __linux__
  std::ifstream input("/proc/meminfo");
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line); std::string name; double kib = 0;
    fields >> name >> kib;
    if (name == "MemTotal:") result.total_gib = kib / 1048576;
    if (name == "MemAvailable:") result.available_gib = kib / 1048576;
  }
  // cgroup-v2 container limits narrow physical capacity, never increase it.
  std::ifstream groups("/proc/self/cgroup");
  while (std::getline(groups, line)) if (line.starts_with("0::")) {
    auto relative = line.substr(3);
    if (relative.find("..") != std::string::npos) continue;
    for (const auto& base : {std::string("/sys/fs/cgroup") + relative, std::string("/sys/fs/cgroup")}) {
      std::ifstream maximum(base + "/memory.max"), current(base + "/memory.current");
      std::string limit; double used = 0;
      if ((maximum >> limit) && limit != "max" && (current >> used)) {
        const double total = std::stod(limit) / 1073741824;
        result.total_gib = std::min(result.total_gib, total);
        result.available_gib = std::min(result.available_gib, std::max(0.0, total - used / 1073741824));
      }
    }
  }
#elif defined(__APPLE__)
  std::uint64_t total = 0; std::size_t length = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &length, nullptr, 0)) throw std::runtime_error("cannot read physical memory");
  vm_statistics64_data_t stats{}; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  const auto host = mach_host_self(); vm_size_t page = 0;
  const auto status = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count);
  const auto pages = host_page_size(host, &page); mach_port_deallocate(mach_task_self(), host);
  if (status != KERN_SUCCESS || pages != KERN_SUCCESS) throw std::runtime_error("cannot read VM memory availability");
  result.total_gib = total / 1073741824.0;
  // Conservative reclaimability estimate, not Activity Monitor's app-memory sum.
  // Do not count the GPU pool separately on a unified-memory machine.
  result.available_gib = (stats.free_count + std::max<double>(stats.purgeable_count, stats.inactive_count * 0.5)) * page / 1073741824.0;
  int pressure = 0; length = sizeof(pressure);
  if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &length, nullptr, 0))
    throw std::runtime_error("cannot read unified memory pressure");
  result.pressure_warning = pressure >= 2;
#else
  throw std::runtime_error("build-memory monitoring supports macOS and Linux/WSL");
#endif
  if (!std::isfinite(result.total_gib) || result.total_gib <= 0 || !std::isfinite(result.available_gib) || result.available_gib < 0)
    throw std::runtime_error("invalid live memory observation");
  result.available_gib = std::min(result.total_gib, result.available_gib);
  return result;
}

BuildBudget choose_build_budget(const MemorySnapshot& memory, double policy, int jobs, int cores) {
  if (!std::isfinite(memory.total_gib) || !std::isfinite(memory.available_gib) ||
      !std::isfinite(policy) || policy <= 0 || memory.total_gib <= 0 || memory.available_gib < 0 ||
      memory.available_gib > memory.total_gib || jobs < 1 || cores < 1)
    throw std::invalid_argument("invalid build-memory inputs");
  BuildBudget budget;
  budget.reserve_gib = std::max(2.0, memory.total_gib * 0.2);
  budget.memory_gib = std::max(0.0, std::min({16.0, policy, memory.total_gib * 0.5,
                                           memory.available_gib - budget.reserve_gib - 0.5}));
  budget.parallel_jobs = std::max(1, std::min({2, jobs, std::max(1, cores / 2),
                                            static_cast<int>(budget.memory_gib / 2)}));
  if (memory.pressure_warning || budget.memory_gib < 2)
    throw std::runtime_error("not enough safe build headroom: free other applications/models and retry; at least 2 GiB plus system reserve is needed");
  return budget;
}

CommandResult run_build_guarded(const std::vector<std::string>& args, const BuildBudget& budget, bool capture) {
  static std::mutex build_mutex;
  std::unique_lock build_lock(build_mutex);
  if (args.empty() || budget.memory_gib <= 0 || budget.memory_gib > 16 || budget.reserve_gib < 2)
    throw std::invalid_argument("invalid guarded build command/budget");
  const auto initial = live_memory_snapshot();
  if (initial.pressure_warning || initial.available_gib < budget.memory_gib + budget.reserve_gib)
    throw std::runtime_error("available memory changed; refusing to launch the build");
  int descriptors[2];
  if (pipe(descriptors)) throw std::runtime_error("build output pipe failed");
  fcntl(descriptors[0], F_SETFD, FD_CLOEXEC); fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
  fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);
  posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attributes, 0);
  std::vector<char*> argv; for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str())); argv.push_back(nullptr);
  pid_t pid = -1;
  const auto spawned = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); close(descriptors[1]);
  if (spawned) { close(descriptors[0]); return {spawned == ENOENT ? 127 : 126, "build spawn failed"}; }
  struct sigaction handler{}, old_int{}, old_term{};
  handler.sa_handler = forward_build_signal; sigemptyset(&handler.sa_mask);
  guarded_pid = pid; interrupted = 0;
  sigaction(SIGINT, &handler, &old_int); sigaction(SIGTERM, &handler, &old_term);
  struct RestoreSignals {
    struct sigaction interrupt, terminate;
    ~RestoreSignals() { guarded_pid = 0; sigaction(SIGINT, &interrupt, nullptr); sigaction(SIGTERM, &terminate, nullptr); }
  } restore{old_int, old_term};
  std::string output, failure; int status = 0; double peak = 0;
  const auto drain = [&] {
    std::array<char, 8192> buffer;
    ssize_t count;
    while ((count = read(descriptors[0], buffer.data(), buffer.size())) > 0) {
      if (capture) { if (output.size() < 8 * 1024 * 1024) output.append(buffer.data(), count); }
      else { std::cout.write(buffer.data(), count); std::cout.flush(); }
    }
  };
  while (true) {
    drain();
    if (interrupted) { failure = "build interrupted"; break; }
    const auto waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) break;
    if (waited < 0 && errno != EINTR) { failure = "cannot monitor build process"; break; }
    try {
      const auto memory = live_memory_snapshot();
      const auto processes = run_command({"ps", "-axo", "pgid=,rss="}, true);
      if (processes.exit_code) throw std::runtime_error("cannot monitor compiler memory");
      std::istringstream rows(processes.output); long group = 0; double kib = 0, total = 0;
      while (rows >> group >> kib) if (group == pid) total += kib;
      const double rss = total / 1048576; peak = std::max(peak, rss);
      if (rss > budget.memory_gib) failure = "compiler process group exceeded its memory budget";
      if (memory.available_gib < budget.reserve_gib || memory.pressure_warning)
        failure = "system/unified-memory headroom exhausted; compilation stopped";
    } catch (const std::exception& error) { failure = error.what(); }
    if (!failure.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (!failure.empty()) {
    kill(-pid, SIGTERM);
    // Keep draining pipes while terminating, then kill the isolated compiler group.
    for (int i = 0; i < 8; ++i) { drain(); std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  }
  drain(); close(descriptors[0]);
  const auto note = "Build peak group RSS: " + std::to_string(peak) + " GiB; cap " + std::to_string(budget.memory_gib) + " GiB.\n";
  if (capture) output += note; else std::cout << note;
  if (!failure.empty()) return {75, output + failure};
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, output};
}
}

#include "mica_server/command.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <spawn.h>
#include <fcntl.h>
#include <mutex>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mica {
namespace {

std::string quote(const std::string& value) {
  if (value.find_first_of(" \t\n'\"") == std::string::npos) return value;
  std::string result = "'";
  for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}

}  // namespace

std::string display_command(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) out << ' ';
    out << quote(args[i]);
  }
  return out.str();
}

CommandResult run_command(const std::vector<std::string>& args, bool capture) {
  if (args.empty()) throw std::runtime_error("cannot run an empty command");
  static std::mutex spawn_mutex;
  std::unique_lock spawn_lock(spawn_mutex);
  int pipefd[2] = {-1, -1};
  if (capture && pipe(pipefd) != 0) throw std::runtime_error("pipe failed");
  if (capture) {
    // Other concurrent subprocesses must not keep each other's pipes open.
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  }

  // Avoid allocations between fork and exec: callers include HTTP threads
  // and parallel download workers, where inherited allocator locks are unsafe.
  std::vector<char*> argv;
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (capture) {
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  }
  pid_t pid = -1;
  const auto spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  spawn_lock.unlock();
  if (spawned != 0) {
    if (capture) { close(pipefd[0]); close(pipefd[1]); }
    return {spawned == ENOENT ? 127 : 126, "process spawn failed"};
  }

  std::string output;
  if (capture) {
    close(pipefd[1]);
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while (true) {
      count = read(pipefd[0], buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(pipefd[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) throw std::runtime_error("waitpid failed");
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return {code, output};
}

bool command_exists(const std::string& command) {
  if (command.find('/') != std::string::npos) {
    return access(command.c_str(), X_OK) == 0;
  }
  const char* raw_path = std::getenv("PATH");
  if (!raw_path) return false;
  std::istringstream paths(raw_path);
  std::string path;
  while (std::getline(paths, path, ':')) {
    if (access((std::filesystem::path(path) / command).c_str(), X_OK) == 0) return true;
  }
  return false;
}

}  // namespace mica


#include "mica_server/command.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include <sys/wait.h>
#include <unistd.h>

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
  int pipefd[2] = {-1, -1};
  if (capture && pipe(pipefd) != 0) throw std::runtime_error("pipe failed");

  const pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork failed");
  if (pid == 0) {
    if (capture) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  std::string output;
  if (capture) {
    close(pipefd[1]);
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while ((count = read(pipefd[0], buffer.data(), buffer.size())) > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(pipefd[0]);
  }
  int status = 0;
  waitpid(pid, &status, 0);
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


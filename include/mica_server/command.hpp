#pragma once

#include <string>
#include <vector>

namespace mica {

struct CommandResult {
  int exit_code{0};
  std::string output;
};

std::string display_command(const std::vector<std::string>& args);
CommandResult run_command(const std::vector<std::string>& args, bool capture = false);
bool command_exists(const std::string& command);

}  // namespace mica


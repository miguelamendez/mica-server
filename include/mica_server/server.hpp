#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mica_server/types.hpp"

namespace mica {

struct ServerOptions {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::filesystem::path root;
  std::filesystem::path config_directory;
  std::optional<Backend> active_backend;
};

int run_server(const Registry& registry, const ServerOptions& options);

}  // namespace mica

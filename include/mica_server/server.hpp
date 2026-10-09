#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "mica_server/types.hpp"

namespace mica {

struct ServerOptions {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::filesystem::path root;
  std::filesystem::path config_directory;
  std::filesystem::path api_key_file;
  std::string api_key;
  std::optional<Backend> active_backend;
  bool ignore_context_limit{false};
  bool allow_partial_workload{false};
};

int run_server(const Registry& registry, const ServerOptions& options);
nlohmann::json server_endpoints();
void download_workload_models(const Registry& registry, const std::filesystem::path& root);

}  // namespace mica

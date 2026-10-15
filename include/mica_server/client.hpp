#pragma once
#include <filesystem>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
namespace mica {
struct ClientOptions {
  std::filesystem::path root, server_root, config, executable;
  int port{55001};
};
nlohmann::json client_detect(const ClientOptions& options);
nlohmann::json client_action(const ClientOptions& options, const nlohmann::json& request);
int client_main(int argc, char** argv);
}

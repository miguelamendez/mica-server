#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
namespace mica {
void validate_server_configuration(const nlohmann::json& document);
nlohmann::json load_server_configuration(const std::filesystem::path& path);
nlohmann::json redact_server_configuration(nlohmann::json document);
nlohmann::json local_server_request(const std::filesystem::path& root,
                                   const std::string& method, const std::string& path,
                                   const nlohmann::json& body);
nlohmann::json local_server_status(const std::filesystem::path& root);
bool local_server_running(const std::filesystem::path& root);
int service_command(const std::string& action, const std::vector<std::string>& arguments,
                    const std::filesystem::path& executable,
                    const std::filesystem::path& default_root);
int configuration_command(const std::vector<std::string>& arguments,
                          const std::filesystem::path& default_root);
}

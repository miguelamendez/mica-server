#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
namespace mica {
void validate_app_manifest(const nlohmann::json& document);
void validate_app_index(const nlohmann::json& document);
nlohmann::json app_inventory(const std::filesystem::path& root, const std::filesystem::path& config);
std::vector<std::string> active_app_workloads(const std::filesystem::path& root);
int app_command(const std::vector<std::string>& arguments, const std::filesystem::path& root,
                const std::filesystem::path& config, const std::filesystem::path& executable);
}

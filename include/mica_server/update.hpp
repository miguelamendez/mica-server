#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
namespace mica {
nlohmann::json select_release_candidate(const nlohmann::json& releases,
                                       const std::string& platform, const std::string& arch);
void validate_update_archive_listing(const std::string& names, const std::string& details,
                                     const std::string& package);
// Payload is staged and validated before replacing the exact installation paths.
void install_update_payload(const std::filesystem::path& payload, const std::filesystem::path& prefix,
                            const std::string& transaction);
int update_command(const std::vector<std::string>& arguments,
                   const std::filesystem::path& executable, const std::filesystem::path& root,
                   const std::string& current_version, const std::string& current_revision);
}

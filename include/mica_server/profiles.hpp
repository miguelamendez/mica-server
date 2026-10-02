#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "mica_server/types.hpp"

namespace mica {

inline constexpr const char* kDefaultProfileCatalogUrl =
    "https://raw.githubusercontent.com/miguelamendez/mica-server/main/profiles/catalog.yaml";

nlohmann::json profile_to_document(const Profile& profile);
nlohmann::json read_profile_file(const std::filesystem::path& path);
void write_profile_file(const std::filesystem::path& path,
                        const nlohmann::json& document);
Profile profile_from_document(Registry& registry, const nlohmann::json& document);
std::string profile_yaml(const nlohmann::json& document);
std::string merge_profile_file(Registry& registry, const std::filesystem::path& path);
void merge_installed_profiles(Registry& registry, const std::filesystem::path& root);
std::filesystem::path install_profile_file(Registry& registry,
                                           const std::filesystem::path& root,
                                           const std::filesystem::path& source);
nlohmann::json fetch_profile_catalog(const std::string& url);
std::filesystem::path install_profile_from_catalog(Registry& registry,
                                                   const std::filesystem::path& root,
                                                   const std::string& id,
                                                   const std::string& url);

}  // namespace mica

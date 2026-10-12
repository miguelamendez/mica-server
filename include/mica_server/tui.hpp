#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
namespace mica {
nlohmann::json tui_inventory(const std::filesystem::path& root, const std::filesystem::path& config,
                             const nlohmann::json& server_override = nullptr);
int run_tui(const std::filesystem::path& executable, const std::filesystem::path& root,
            const std::filesystem::path& config, bool snapshot = false);
}

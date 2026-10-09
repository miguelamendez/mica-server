#pragma once
#include <filesystem>
namespace mica {
int run_tui(const std::filesystem::path& executable, const std::filesystem::path& root,
            const std::filesystem::path& config, bool snapshot = false);
}

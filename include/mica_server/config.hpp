#pragma once

#include <filesystem>

#include "mica_server/types.hpp"

namespace mica {

Registry load_registry(const std::filesystem::path& config_directory);

}  // namespace mica


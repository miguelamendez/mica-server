#pragma once

#include <filesystem>

#include "mica_server/types.hpp"

namespace mica {

Registry load_registry(const std::filesystem::path& config_directory,
                       bool ignore_context_limit = false,
                       bool allow_partial_workload = false);

}  // namespace mica

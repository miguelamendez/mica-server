#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "mica_server/types.hpp"

namespace mica {

HardwareInfo detect_hardware();
HardwareInfo hardware_from_json(const nlohmann::json& profile);
nlohmann::json hardware_to_json(const HardwareInfo& hardware);
HardwareInfo load_hardware_profile(const std::filesystem::path& path);
void write_hardware_profile(const HardwareInfo& hardware,
                            const std::filesystem::path& path);

}  // namespace mica

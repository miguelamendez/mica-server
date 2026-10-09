#pragma once
#include <filesystem>
#include "mica_server/types.hpp"

namespace mica {
// Read-only inventory. Compatibility is not inference certification or readiness.
std::filesystem::path model_cache_directory(const std::filesystem::path& root,
    const ModelDefinition& model, Backend backend, const std::string& engine);
nlohmann::json inspect_engine(const EngineDefinition& engine, const HardwareInfo& hardware,
    const std::filesystem::path& root);
nlohmann::json inspect_artifact(const ModelDefinition& model, const Artifact& artifact,
    Backend backend, Quantization quantization, const std::filesystem::path& root);
nlohmann::json inspect_workload(const Registry& registry, const Profile& profile,
    const HardwareInfo& hardware, const std::filesystem::path& root);
}

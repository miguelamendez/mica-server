#pragma once

#include "mica_server/types.hpp"

namespace mica {

// MB = 10^6 bytes; GiB = 2^30 bytes. Model sizes have no device placement.
nlohmann::json estimate_artifact_memory(const Artifact& artifact,
    int total_tokens, int sequences = 1, const std::string& key_type = "q8",
    const std::string& value_type = "q8", const std::string& speculative = "auto",
    int micro_batch_tokens = 512);
nlohmann::json model_memory_report(const ModelDefinition& model);
nlohmann::json workload_memory_report(const Registry& registry, const Profile& profile,
    double budget_gib = 0.0, double global_limit_gib = 0.0);
void validate_memory_strategy(const Registry& registry, const Profile& profile,
    double global_limit_gib);
void apply_residency_strategy(Profile& profile);

}  // namespace mica

#pragma once

#include <optional>
#include <vector>

#include "mica_server/types.hpp"

namespace mica {

StartupPlan plan_startup(const Registry& registry, const Profile& profile,
                         Backend backend, Quantization quantization,
                         double max_ram_gib);

std::vector<ResidentModel> rank_eviction_candidates(
    std::vector<ResidentModel> residents);

// Translate mica's absolute memory limit into vLLM's device-fraction flag.
// Metal uses unified memory, so the RAM limit is the accelerator limit. Discrete
// GPU backends use the explicit VRAM limit. CPU and TPU runtimes do not consume
// this flag.
std::optional<double> vllm_memory_utilization(
    VllmDevice device, double max_ram_gib, double max_vram_gib,
    double accelerator_memory_gib);

}  // namespace mica

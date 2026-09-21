#pragma once

#include <optional>
#include <vector>

#include "mica_server/types.hpp"

namespace mica {

struct ResolvedModelPlacement {
  std::string device{"cpu"};
  int gpu_layers{0};
  double ram_reservation_gib{0.0};
  double vram_reservation_gib{0.0};
  bool unified_memory{false};
};

// Resolve a profile's logical placement (for example accelerator:0) to the
// engine-specific device exposed by the detected hardware. Unified-memory
// accelerators consume only the RAM pool; discrete accelerators consume both
// host RAM and their explicit VRAM pool.
ResolvedModelPlacement resolve_model_placement(
    const ProfileModel& policy, const Artifact& artifact,
    const HardwareInfo& hardware);

StartupPlan plan_startup(const Registry& registry, const Profile& profile,
                         Backend backend, Quantization quantization,
                         double max_ram_gib);

StartupPlan plan_profile_startup(const Registry& registry, const Profile& profile,
                                 Backend backend, Quantization quantization,
                                 double max_ram_gib);

StartupPlan plan_profile_startup_resources(
    const Registry& registry, const Profile& profile, Backend backend,
    Quantization quantization, double max_ram_gib, double max_vram_gib,
    const HardwareInfo& hardware);

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

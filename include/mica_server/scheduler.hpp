#pragma once

#include <vector>

#include "mica_server/types.hpp"

namespace mica {

StartupPlan plan_startup(const Registry& registry, const Profile& profile,
                         Backend backend, Quantization quantization,
                         double max_ram_gib);

std::vector<ResidentModel> rank_eviction_candidates(
    std::vector<ResidentModel> residents);

}  // namespace mica


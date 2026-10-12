#pragma once
#include "mica_server/command.hpp"
#include <vector>
#include <string>
namespace mica {
struct MemorySnapshot {
  double total_gib{0}, available_gib{0};
  bool pressure_warning{false};
};
struct BuildBudget {
  double memory_gib{0}, reserve_gib{0};
  int parallel_jobs{1};
};
MemorySnapshot live_memory_snapshot();
BuildBudget choose_build_budget(const MemorySnapshot& memory, double policy_limit_gib,
                               int policy_jobs, int cpu_cores);
CommandResult run_build_guarded(const std::vector<std::string>& command,
                                const BuildBudget& budget, bool capture = false);
}

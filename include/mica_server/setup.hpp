#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <map>
#include <vector>

#include "mica_server/types.hpp"
#include "mica_server/machine.hpp"

namespace mica {

struct SetupOptions {
  std::filesystem::path root;
  std::filesystem::path config_directory;
  std::filesystem::path hardware_profile;
  std::filesystem::path machine_policy_file;
  std::filesystem::path api_key_file;
  std::filesystem::path profile_file;
  std::string profile{"auto"};
  std::string hf_repo;
  std::vector<Backend> backends;
  std::vector<Quantization> quantizations{Quantization::q4};
  bool backends_explicit{false};
  bool quantizations_explicit{false};
  double max_ram_gib{8.0};
  double max_vram_gib{0.0};
  bool max_vram_explicit{false};
  VllmDevice vllm_device{VllmDevice::automatic};
  bool dry_run{false};
  bool refresh{false};
  bool ignore_context_limit{false};
  bool allow_partial_workload{false};
};

struct ResolvedSetup {
  HardwareInfo hardware;
  MachinePolicy source_machine_policy;
  ResolvedMachinePolicy machine;
  std::filesystem::path machine_policy_file;
  std::vector<Backend> backends;
  VllmDevice vllm_device{VllmDevice::automatic};
  SetupOptions options;
  std::map<Backend, std::map<Quantization, StartupPlan>> startups;
};

ResolvedSetup resolve_setup(Registry& registry, SetupOptions options);
void execute_setup(const Registry& registry, const ResolvedSetup& setup);
std::string generate_api_key();

}  // namespace mica

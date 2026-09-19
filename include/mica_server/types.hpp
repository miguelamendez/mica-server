#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mica {

enum class Backend { mlx, gguf, vllm };
enum class Quantization { q4, q8, native };
enum class VllmDevice { automatic, cpu, cuda, metal, rocm, xpu, tpu };
enum class Residency { pinned, warm, on_demand, ephemeral };

std::string to_string(Backend value);
std::string to_string(Quantization value);
Backend parse_backend(const std::string& value);
Quantization parse_quantization(const std::string& value);
std::string to_string(VllmDevice value);
VllmDevice parse_vllm_device(const std::string& value);
std::string to_string(Residency value);
Residency parse_residency(const std::string& value);

struct HardwareInfo {
  struct Accelerator {
    std::string id;
    std::string type;
    std::string vendor;
    std::string name;
    std::string runtime;
    std::string architecture;
    std::string driver;
    double memory_gib{0.0};
    bool unified_memory{false};
    std::vector<std::string> apis;
  };

  std::string os;
  std::string os_version;
  std::string arch;
  std::string cpu_vendor;
  std::string cpu_model;
  int physical_cpu_cores{0};
  int logical_cpu_cores{0};
  bool apple_silicon{false};
  bool wsl{false};
  double ram_gib{0.0};
  double unified_memory_gib{0.0};
  std::vector<Accelerator> accelerators;
  std::string mlx_target{"unsupported"};
  std::string gguf_target{"cpu"};
  std::string audio_target{"cpu"};
  std::string vllm_target{"cpu"};
  std::map<std::string, bool> toolchains;
  double build_memory_limit_gib{16.0};
  int build_parallelism{2};
  bool nvidia_detected{false};
  std::vector<double> nvidia_vram_gib;

  [[nodiscard]] bool has_runtime(const std::string& runtime) const;
  [[nodiscard]] double largest_memory_gib(const std::string& runtime) const;
  [[nodiscard]] bool supports_mlx() const;
  [[nodiscard]] bool supports_gguf() const;
  [[nodiscard]] bool supports_vllm() const;
  [[nodiscard]] Backend recommended_backend() const;
  [[nodiscard]] VllmDevice recommended_vllm_device() const;
};

struct Artifact {
  bool supported{false};
  std::string engine;
  std::string format;
  std::string quantization_type;
  std::string pattern;
  std::string repository_pattern;
  std::string reason;
  std::uint64_t size_bytes{0};
  std::uint64_t projector_size_bytes{0};
  std::string size_source;
  double reservation_gib{0.0};
  std::string projector_pattern;
  std::string projector_repository_pattern;
};

struct ModelDefinition {
  std::string id;
  std::string capability;
  std::string description;
  bool catalog_visible{true};
  std::vector<std::string> tags;
  std::string source_repo;
  std::string mlx_converter;
  std::string mlx_quantization_profile;
  std::string gguf_family;
  int gguf_context_tokens{8192};
  int gguf_parallel_slots{1};
  bool mlx_extract_mtp{false};
  std::map<Backend, std::string> repositories;
  std::map<Backend, std::string> repository_revisions;
  int startup_priority{100};
  bool required{false};
  std::map<Backend, bool> required_by_backend;
  std::map<Backend, std::map<Quantization, Artifact>> artifacts;

  [[nodiscard]] bool required_for(Backend backend) const {
    const auto found = required_by_backend.find(backend);
    return found == required_by_backend.end() ? required : found->second;
  }
};

struct ProfileModel {
  std::string id;
  std::string execution;
  std::string engine;
  Backend backend{Backend::gguf};
  Quantization quantization{Quantization::q4};
  Residency residency{Residency::on_demand};
  int priority{50};
  bool startup{false};
  int idle_seconds{300};
  int max_input_tokens{7168};
  int max_output_tokens{1024};
  int max_total_tokens{8192};
  int max_concurrent_requests{1};
  std::string kv_cache_precision{"q8"};
};

struct Profile {
  std::string name;
  int schema{1};
  std::string mode{"interactive"};
  bool catalog_visible{true};
  Quantization quantization{Quantization::q4};
  std::vector<std::string> models;
  std::optional<Backend> backend;
  int max_input_tokens{7168};
  int max_output_tokens{1024};
  int max_total_tokens{8192};
  int max_concurrent_requests{1};
  std::string kv_cache_precision{"q8"};
  double maximum_ram_gib{0.0};
  double maximum_vram_gib{0.0};
  double memory_safety_reserve_gib{0.0};
  int maximum_resident_workers{0};
  std::vector<ProfileModel> model_policies;

  [[nodiscard]] const ProfileModel* policy_for(const std::string& id) const {
    for (const auto& policy : model_policies) {
      if (policy.id == id) return &policy;
    }
    return nullptr;
  }
};

struct Policy {
  int idle_ttl_seconds{300};
  double safety_margin{1.15};
  int min_system_available_ram_gib{6};
  int load_timeout_seconds{180};
  int queue_timeout_seconds{60};
};

struct VlmToolDefinition {
  bool enabled{false};
  std::string name{"vlm_tool"};
  std::string description;
  std::string model_id{"minicpm-v46-thinking"};
  int max_images_per_call{8};
  int max_videos_per_call{1};
  int max_document_pages_per_call{8};
  int max_video_frames{32};
  int max_total_visual_items{8};
  int max_agent_steps{4};
  std::uint64_t max_upload_bytes{50ULL * 1024ULL * 1024ULL};
};

struct Registry {
  std::string default_hf_repo;
  std::string llama_cpp_revision;
  std::string audio_cpp_revision;
  std::vector<ModelDefinition> models;
  std::map<std::string, Profile> profiles;
  Policy policy;
  VlmToolDefinition vlm_tool;

  [[nodiscard]] const ModelDefinition& model(const std::string& id) const;
  [[nodiscard]] const Profile& profile(const std::string& name) const;
};

struct ResidentModel {
  std::string id;
  double reservation_gib{0.0};
  std::uint64_t last_used_monotonic_ns{0};
  bool ttl_expired{false};
  int in_flight{0};
};

struct StartupPlan {
  std::vector<std::string> admitted;
  std::vector<std::string> skipped;
  double reserved_gib{0.0};
  std::optional<std::string> error;
};

}  // namespace mica

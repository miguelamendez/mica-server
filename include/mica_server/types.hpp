#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mica {

enum class Backend { mlx, gguf, vllm };
// Artifact variant IDs are intentionally open-ended. Built-in producers use
// q4/q8/native, while pre-packed artifacts can use precise identifiers such as
// pq2_0 without pretending to be another quantization family.
struct Quantization {
  std::string id{"q4"};

  Quantization() = default;
  explicit Quantization(std::string value) : id(std::move(value)) {}
  auto operator<=>(const Quantization&) const = default;

  static const Quantization q4;
  static const Quantization q8;
  static const Quantization native;
};
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
  struct File {
    std::string role;
    std::string path;
    std::string repository_path;
    std::string repository;
    std::string revision;
    std::uint64_t size_bytes{0};
    std::string sha256;
  };
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
  std::uint64_t kv_bytes_per_token_f16{0};
  std::string projector_pattern;
  std::string projector_repository_pattern;
  std::string sha256;
  std::string projector_sha256;
  std::vector<File> files;
  std::vector<std::string> required_features;
  // First engine commit known to support this artifact's architecture.
  std::string minimum_engine_commit;
};

struct EngineDefinition {
  struct EndpointContract {
    std::string id;
    std::string operation;
    std::string transport{"http"};
    std::string adapter_call;
    std::string method{"POST"};
    std::string path;
    std::vector<std::string> required_inputs;
    std::vector<std::string> optional_inputs;
    std::vector<std::string> outputs;
    bool streaming{false};
    bool supports_tools{false};
  };
  std::string id;
  Backend backend{Backend::gguf};
  std::string status{"current"};
  std::string installer;
  std::string launcher;
  std::string device_target;
  std::string source_url;
  std::string revision;
  std::string runtime_directory;
  std::string server_executable;
  std::string quantizer_executable;
  std::vector<std::string> build_targets;
  std::vector<std::string> version_arguments;
  std::vector<std::string> hardware;
  std::vector<std::string> artifact_formats;
  std::vector<std::string> features;
  std::vector<EndpointContract> endpoint_contracts;
  std::map<std::string, std::map<std::string, std::string>> cmake_definitions;
  std::string environment_group;
  std::vector<std::string> python_packages;
};

struct ModelDefinition {
  struct Reference {
    std::string kind;
    std::string title;
    std::string url;
    std::string description;
  };
  struct Interaction {
    std::string operation;
    std::vector<std::string> required_inputs;
    std::vector<std::string> optional_inputs;
    std::vector<std::string> outputs;
  };
  struct TokenLimitClaim {
    int tokens{0};
    std::string source;
  };
  std::string id;
  std::string capability;
  std::string description;
  bool catalog_visible{true};
  std::vector<std::string> tags;
  std::string source_repo;
  std::vector<Reference> references;
  std::string mlx_converter;
  std::string mlx_quantization_profile;
  std::string gguf_family;
  int gguf_context_tokens{8192};
  std::optional<TokenLimitClaim> trained_context_tokens;
  std::optional<TokenLimitClaim> useful_context_tokens;
  std::optional<TokenLimitClaim> supported_output_tokens;
  std::optional<TokenLimitClaim> trained_output_tokens;
  std::vector<std::string> input_modalities;
  std::vector<std::string> output_modalities;
  std::vector<std::string> abilities;
  std::vector<Interaction> supported_interactions;
  std::vector<std::string> tool_call_formats;
  int gguf_parallel_slots{1};
  std::vector<std::string> thinking_modes;
  bool thinking_budget_supported{false};
  bool mlx_extract_mtp{false};
  std::map<Backend, std::string> repositories;
  std::map<Backend, std::string> repository_revisions;
  int startup_priority{100};
  bool required{false};
  std::map<Backend, bool> required_by_backend;
  std::map<Backend, std::map<Quantization, Artifact>> artifacts;
  std::map<std::string, std::map<Quantization, Artifact>> engine_artifacts;
  std::vector<std::string> engine_order;
  [[nodiscard]] const Artifact& artifact_for(Backend backend, Quantization quant,
                                            const std::string& engine = {}) const;

  [[nodiscard]] bool required_for(Backend backend) const {
    const auto found = required_by_backend.find(backend);
    return found == required_by_backend.end() ? required : found->second;
  }
};

struct ProfileModel {
  std::string id;
  std::string execution;
  std::string engine;
  bool engine_explicit{true};
  bool artifact_explicit{true};
  std::string device_target;
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
  // Placement is resolved per worker. "auto" follows the detected engine
  // target; "fixed" requires device to be cpu, accelerator:N, or runtime:N.
  std::string placement_mode{"auto"};
  std::string device{"auto"};
  int gpu_layers{-1};
  double ram_reservation_gib{-1.0};
  double vram_reservation_gib{-1.0};
};

struct Profile {
  std::string name;
  std::string description;
  std::string default_chat_model;
  // Optional defaults for text, image, video, asr, and tts requests.
  std::map<std::string, std::string> default_models;
  std::string engine_policy{"prefer-installed"};
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
  double required_ram_gib{0.0};
  double required_vram_gib{0.0};
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
  std::string runtime_root;
  std::optional<HardwareInfo> resolution_hardware;
  std::string default_hf_repo;
  std::vector<ModelDefinition> models;
  std::map<std::string, EngineDefinition> engines;
  std::map<std::string, Profile> profiles;
  Policy policy;
  VlmToolDefinition vlm_tool;

  [[nodiscard]] const ModelDefinition& model(const std::string& id) const;
  [[nodiscard]] const EngineDefinition& engine(const std::string& id) const;
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
  double reserved_ram_gib{0.0};
  double reserved_vram_gib{0.0};
  std::map<std::string, double> reserved_vram_by_device_gib;
  std::optional<std::string> error;
};

}  // namespace mica

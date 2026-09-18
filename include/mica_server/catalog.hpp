#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "mica_server/types.hpp"

namespace mica {

struct AddModelOptions {
  std::filesystem::path root;
  std::string url;
  std::string id;
  std::string modality;
  std::string description;
  bool dry_run{false};
};

struct QuantizeModelOptions {
  std::filesystem::path root;
  std::filesystem::path config_directory;
  std::string id;
  std::string source_revision;
  Backend backend{Backend::gguf};
  Quantization quantization{Quantization::q4};
  int group_size{64};
  std::string vllm_algorithm;
  std::string vllm_scheme;
  std::string quantization_device{"auto"};
  std::string calibration_dataset;
  std::string calibration_dataset_split;
  int calibration_samples{0};
  int calibration_sequence_length{0};
  int calibration_batch_size{0};
  int auto_round_iterations{0};
  bool dry_run{false};
};

struct ConfigureVllmModelOptions {
  std::filesystem::path root;
  std::string id;
  double reservation_gib{0.0};
  bool dry_run{false};
};

std::string normalize_modality(const std::string& value);
std::string modality_capability(const std::string& modality);
std::string add_custom_model(const AddModelOptions& options);
void quantize_model(const QuantizeModelOptions& options);
void configure_vllm_custom_model(const ConfigureVllmModelOptions& options);
void merge_custom_models(Registry& registry, const std::filesystem::path& root);
nlohmann::json registry_catalog(
    const Registry& registry,
    const std::optional<std::string>& capability = std::nullopt,
    const std::optional<Backend>& backend = std::nullopt,
    bool check_remote = false);

}  // namespace mica

#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

}  // namespace mica

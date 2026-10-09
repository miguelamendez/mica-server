#include "mica_server/profiles.hpp"
#include "mica_server/inventory.hpp"
#include "mica_server/memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <sys/stat.h>

#include "mica_server/catalog.hpp"
#include "mica_server/command.hpp"
#include "mica_server/hardware.hpp"

namespace mica {
namespace {

using json = nlohmann::json;

void reject_unknown_fields(
    const json& object, std::initializer_list<std::string_view> allowed,
    std::string_view context) {
  if (!object.is_object()) {
    throw std::invalid_argument(std::string(context) + " must be an object");
  }
  for (auto entry = object.begin(); entry != object.end(); ++entry) {
    const auto known = std::any_of(
        allowed.begin(), allowed.end(),
        [&](const auto field) { return field == entry.key(); });
    if (!known) {
      throw std::invalid_argument(std::string(context) +
                                  " contains unknown field: " + entry.key());
    }
  }
}

bool safe_id(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' || character == '_' ||
                  character == '.';
         });
}

std::string normalize_repository(std::string value) {
  constexpr std::string_view prefix = "https://huggingface.co/";
  if (value.rfind(prefix, 0) == 0) value.erase(0, prefix.size());
  while (!value.empty() && value.back() == '/') value.pop_back();
  const auto slash = value.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= value.size() ||
      value.find('/', slash + 1) != std::string::npos ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_' ||
               character == '.' || character == '/';
      })) {
    throw std::invalid_argument(
        "Hugging Face repository must be OWNER/REPO or its canonical URL");
  }
  return value;
}

bool immutable_revision(const std::string& value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isxdigit(character);
         });
}

bool commercial_license(const std::string& value) {
  static const std::set<std::string> allowed = {
      "apache-2.0", "mit", "bsd", "bsd-2-clause", "bsd-3-clause",
      "isc", "cc0-1.0", "unlicense", "mpl-2.0", "openmdw-1.1"};
  auto normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return allowed.contains(normalized);
}

std::string format_for_backend(Backend backend) {
  if (backend == Backend::mlx) return "mlx";
  if (backend == Backend::gguf) return "gguf";
  return "safetensors";
}

const ProfileModel& execution_template(const Registry& registry,
                                       const std::string& execution) {
  for (const auto& [_, profile] : registry.profiles) {
    for (const auto& policy : profile.model_policies) {
      if (policy.execution == execution) return policy;
    }
  }
  throw std::invalid_argument("unknown execution profile: " + execution);
}

void finalize_profile(Profile& profile) {
  if (profile.model_policies.empty()) {
    throw std::invalid_argument("profile must contain at least one model");
  }
  const auto& first = profile.model_policies.front();
  profile.quantization = first.quantization;
  profile.max_input_tokens = first.max_input_tokens;
  profile.max_output_tokens = first.max_output_tokens;
  profile.max_total_tokens = first.max_total_tokens;
  profile.max_concurrent_requests = first.max_concurrent_requests;
  profile.kv_cache_precision = first.kv_cache_precision;
  profile.backend.reset();
  if (std::all_of(profile.model_policies.begin(), profile.model_policies.end(),
                  [&](const auto& item) { return item.backend == first.backend; })) {
    profile.backend = first.backend;
  }
}

void validate_policy(const Registry& registry, const Profile& profile,
                     const ProfileModel& policy) {
  const auto& model = registry.model(policy.id);
  const auto backend = model.artifacts.find(policy.backend);
  if (backend == model.artifacts.end()) {
    throw std::invalid_argument("model has no selected backend: " + policy.id);
  }
  const auto& selected_artifact = model.artifact_for(policy.backend, policy.quantization, policy.engine);
  if (!selected_artifact.supported) {
    throw std::invalid_argument("model has no selected artifact: " + policy.id + "@" +
                                to_string(policy.backend) + ":" +
                                to_string(policy.quantization));
  }
  if (selected_artifact.engine != policy.engine) {
    throw std::invalid_argument("selected artifact requires engine " +
                                selected_artifact.engine + ", not " + policy.engine);
  }
  if (policy.max_input_tokens < 1 ||
      (model.capability == "embedding" ? policy.max_output_tokens != 0 : policy.max_output_tokens < 1) ||
      policy.max_total_tokens < policy.max_input_tokens + policy.max_output_tokens) {
    throw std::invalid_argument("inconsistent context limits for " + policy.id);
  }
  if ((model.capability == "text" || model.capability == "vision" || model.capability == "embedding") &&
      !registry.ignore_context_limit &&
      policy.max_total_tokens > model.gguf_context_tokens) {
    throw std::invalid_argument("profile exceeds declared context for " + policy.id);
  }
  if (model.max_output_tokens &&
      policy.max_output_tokens > *model.max_output_tokens) {
    throw std::invalid_argument("profile exceeds supported output for " + policy.id);
  }
  if (policy.max_concurrent_requests < 1 || policy.max_concurrent_requests > 64) {
    throw std::invalid_argument("invalid concurrency for " + policy.id);
  }
  static const std::set<std::string> cache_precisions{
      "q4", "q8", "auto", "runtime-managed", "not-applicable"};
  if (!cache_precisions.contains(policy.kv_cache_precision)) {
    throw std::invalid_argument("unsupported KV-cache precision for " + policy.id);
  }
  for (const auto& precision : {policy.kv_cache_k_precision, policy.kv_cache_v_precision}) {
    if (!precision.empty() && (policy.backend != Backend::gguf ||
        (precision != "q4" && precision != "q8" && precision != "f16")))
      throw std::invalid_argument("independent K/V precision requires GGUF and q4/q8/f16");
  }
  if (policy.token_batch_size < 0 || policy.token_batch_size > 8192 ||
      policy.micro_batch_size < 0 || policy.micro_batch_size > 8192 ||
      (policy.micro_batch_size > 0 &&
       (policy.token_batch_size == 0 || policy.micro_batch_size > policy.token_batch_size)) ||
      policy.context_checkpoints < -1 || policy.context_checkpoints > 64 ||
      policy.ram_cache_mib < -1 || policy.ram_cache_mib > 32768 ||
      policy.draft_max_tokens < 1 || policy.draft_max_tokens > 32 ||
      policy.draft_gpu_layers < -1 || policy.draft_gpu_layers > 999 ||
      (policy.draft_kv_cache_precision != "q4" && policy.draft_kv_cache_precision != "q8") ||
      (policy.speculative_method != "auto" && policy.speculative_method != "none" &&
       policy.speculative_method != "mtp" && policy.speculative_method != "dflash")) {
    throw std::invalid_argument("invalid native inference options for " + policy.id);
  }
  if (profile_native_options(policy) != profile_native_options(ProfileModel{}) &&
      (profile.schema < 5 || registry.engine(policy.engine).launcher != "llama-server")) {
    throw std::invalid_argument("native inference options require a schema-5 llama-server workload");
  }
  if (policy.projector_on_cpu && selected_artifact.projector_pattern.empty()) {
    throw std::invalid_argument("CPU projector placement requires a projector");
  }
  if (policy.speculative_method == "mtp" || policy.speculative_method == "dflash") {
    const auto role = policy.speculative_method + "-drafter";
    if (std::none_of(selected_artifact.files.begin(), selected_artifact.files.end(),
                     [&](const auto& file) { return file.role == role; })) {
      throw std::invalid_argument("speculative method requires its registered drafter: " + policy.id);
    }
  }
  if (policy.priority < 0 || policy.priority > 1000 || policy.idle_seconds < 0) {
    throw std::invalid_argument("invalid residency policy for " + policy.id);
  }
  if (profile.required_ram_gib < 0 || profile.required_vram_gib < 0 ||
      profile.memory_safety_reserve_gib < 0 ||
      profile.maximum_resident_workers < 0 ||
      profile.maximum_resident_workers > 128) {
    throw std::invalid_argument("profile memory values cannot be negative");
  }
  if (policy.placement_mode != "auto" && policy.placement_mode != "fixed") {
    throw std::invalid_argument("placement mode must be auto or fixed for " + policy.id);
  }
  if (policy.placement_mode == "fixed" && policy.device == "auto") {
    throw std::invalid_argument("fixed placement requires a device for " + policy.id);
  }
  if (policy.device != "auto" && policy.device != "cpu" &&
      !policy.device.starts_with("accelerator:") &&
      !policy.device.starts_with("cuda:") &&
      !policy.device.starts_with("rocm:") &&
      !policy.device.starts_with("xpu:") &&
      !policy.device.starts_with("metal:")) {
    throw std::invalid_argument("unsupported placement device for " + policy.id);
  }
  if (policy.gpu_layers < -1 || policy.gpu_layers > 999 ||
      policy.ram_reservation_gib < -1 || policy.vram_reservation_gib < -1) {
    throw std::invalid_argument("invalid placement resource values for " + policy.id);
  }
  if (policy.device == "cpu" && policy.gpu_layers > 0) {
    throw std::invalid_argument("CPU placement cannot request GPU layers for " + policy.id);
  }
}

double conservative_reservation(const json& artifact, Quantization quantization) {
  const double size = artifact.value("size_gib", 0.0);
  if (size > 0) return std::max(1.0, size * 1.4 + 0.5);
  return quantization == Quantization::q4 ? 4.0 :
         quantization == Quantization::q8 ? 8.0 : 12.0;
}

ModelDefinition external_model(const Registry& registry, const json& entry,
                               ProfileModel& policy) {
  if (!entry.contains("source") || !entry.at("source").is_object()) {
    throw std::invalid_argument("external model requires a source object");
  }
  const auto& source = entry.at("source");
  reject_unknown_fields(source,
                        {"repository", "revision", "license",
                         "trust_remote_code"},
                        "profile model source");
  ModelDefinition model;
  model.id = entry.at("id").get<std::string>();
  model.source_repo = normalize_repository(source.at("repository").get<std::string>());
  const auto revision = source.at("revision").get<std::string>();
  if (!immutable_revision(revision)) {
    throw std::invalid_argument("external model revision must be an immutable commit SHA");
  }
  const auto license = source.at("license").get<std::string>();
  if (!commercial_license(license)) {
    throw std::invalid_argument("external model license is not on the commercial-use allowlist");
  }
  if (source.value("trust_remote_code", false)) {
    throw std::invalid_argument("trust_remote_code is disabled for profile models");
  }
  const auto modality = normalize_modality(entry.at("modality").get<std::string>());
  model.capability = modality_capability(modality);
  if (model.capability == "asr") {
    model.input_modalities = {"audio"};
    model.output_modalities = {"text"};
  } else if (model.capability == "tts") {
    model.input_modalities = {"text", "audio"};
    model.output_modalities = {"audio"};
  } else if (model.capability == "vision") {
    model.input_modalities = {"text", "image"};
    const auto declared = entry.at("modality").get<std::string>();
    if (declared.find("video") != std::string::npos) {
      model.input_modalities.push_back("video");
    }
    model.output_modalities = {"text"};
  } else {
    model.input_modalities = {"text"};
    model.output_modalities = {"text"};
  }
  model.description = entry.value("description", std::string("External profile model"));
  model.tags = entry.value("tags", std::vector<std::string>{});
  model.tags.push_back(modality);
  model.tags.push_back("license:" + license);
  model.tags.push_back("profile-external");
  model.gguf_context_tokens = entry.value("declared_context_tokens", 2304);
  model.startup_priority = entry.value("priority", 50);

  // Unknown architectures start from a deliberately small request envelope.
  // A profile author may raise it explicitly, but it still cannot exceed the
  // model's declared local context ceiling.
  policy.max_input_tokens = 2048;
  policy.max_output_tokens = 256;
  policy.max_total_tokens = 2304;
  policy.max_concurrent_requests = 1;
  policy.kv_cache_precision = "q8";
  policy.engine = entry.at("engine").get<std::string>();
  const auto& engine = registry.engine(policy.engine);
  if (engine.status != "current" && engine.status != "candidate") {
    throw std::invalid_argument("profile engine is not runnable: " + policy.engine);
  }
  policy.backend = engine.backend;
  policy.device_target = engine.device_target;
  const auto& artifact_document = entry.at("artifact");
  reject_unknown_fields(
      artifact_document,
      {"repository", "revision", "format", "quantization",
       "quantization_type", "path", "projector", "size_bytes", "size_gib",
       "projector_size_bytes", "reservation_gib", "size_source", "sha256",
       "projector_sha256"},
      "profile model artifact");
  const auto expected_format = format_for_backend(policy.backend);
  const auto format = artifact_document.value("format", expected_format);
  if (std::find(engine.artifact_formats.begin(), engine.artifact_formats.end(),
                format) == engine.artifact_formats.end()) {
    throw std::invalid_argument("artifact format does not match the selected engine");
  }
  policy.quantization = parse_quantization(
      artifact_document.value("quantization", std::string("q4")));
  Artifact artifact;
  artifact.supported = true;
  artifact.engine = policy.engine;
  artifact.format = format;
  artifact.quantization_type = artifact_document.value(
      "quantization_type", to_string(policy.quantization));
  artifact.pattern = artifact_document.at("path").get<std::string>();
  artifact.repository_pattern = artifact.pattern;
  artifact.projector_pattern = artifact_document.value("projector", std::string());
  artifact.projector_repository_pattern = artifact.projector_pattern;
  artifact.reservation_gib = artifact_document.value(
      "reservation_gib", conservative_reservation(artifact_document,
                                                    policy.quantization));
  if (artifact_document.contains("size_bytes")) {
    artifact.size_bytes = artifact_document.at("size_bytes").get<std::uint64_t>();
  } else if (artifact_document.contains("size_gib")) {
    artifact.size_bytes = static_cast<std::uint64_t>(std::llround(
        artifact_document.at("size_gib").get<double>() * 1024.0 * 1024.0 * 1024.0));
  }
  if (artifact_document.contains("projector_size_bytes")) {
    artifact.projector_size_bytes =
        artifact_document.at("projector_size_bytes").get<std::uint64_t>();
  }
  artifact.size_source = artifact_document.value(
      "size_source", artifact.size_bytes > 0 ? "profile-declared" : "unknown");
  artifact.sha256 = artifact_document.value("sha256", std::string());
  artifact.projector_sha256 =
      artifact_document.value("projector_sha256", std::string());
  const auto valid_hash = [](const std::string& value) {
    return value.empty() ||
           (value.size() == 64 &&
            std::all_of(value.begin(), value.end(), [](unsigned char character) {
              return std::isdigit(character) ||
                     (character >= 'a' && character <= 'f');
            }));
  };
  if (!valid_hash(artifact.sha256) || !valid_hash(artifact.projector_sha256)) {
    throw std::invalid_argument("artifact SHA-256 must contain 64 lowercase hex characters");
  }
  if (artifact.pattern.empty() || artifact.pattern.front() == '/' ||
      artifact.pattern.find("..") != std::string::npos) {
    throw std::invalid_argument("artifact path must be a safe repository-relative path");
  }
  if (policy.backend == Backend::gguf &&
      std::filesystem::path(artifact.pattern).extension() != ".gguf") {
    throw std::invalid_argument("GGUF engine requires a .gguf artifact");
  }
  if (policy.backend == Backend::gguf && model.capability == "vision" &&
      artifact.projector_pattern.empty()) {
    throw std::invalid_argument("GGUF vision model requires an explicit projector");
  }
  if (engine.launcher == "audio-server") {
    model.gguf_family = entry.value("family", std::string());
    if (model.gguf_family.empty()) {
      throw std::invalid_argument("audio-cpp model requires a family adapter name");
    }
  }
  const auto artifact_repo = artifact_document.contains("repository")
                                 ? normalize_repository(
                                       artifact_document.at("repository").get<std::string>())
                                 : model.source_repo;
  const auto artifact_revision = artifact_document.value("revision", revision);
  if (!immutable_revision(artifact_revision)) {
    throw std::invalid_argument("artifact revision must be an immutable commit SHA");
  }
  model.repositories[policy.backend] = artifact_repo;
  model.repository_revisions[policy.backend] = artifact_revision;
  model.artifacts[policy.backend][policy.quantization] = artifact;
  return model;
}

void require_yaml_path(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  if (extension != ".yaml" && extension != ".yml") {
    throw std::invalid_argument(
        "profile files must use a .yaml or .yml extension: " + path.string());
  }
}

json scalar_to_json(const YAML::Node& node) {
  const auto value = node.Scalar();
  if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") return value;
  if (value == "null" || value == "Null" || value == "NULL" || value == "~") {
    return nullptr;
  }
  if (value == "true" || value == "True" || value == "TRUE") return true;
  if (value == "false" || value == "False" || value == "FALSE") return false;

  std::int64_t signed_value = 0;
  const auto signed_result = std::from_chars(
      value.data(), value.data() + value.size(), signed_value);
  if (signed_result.ec == std::errc{} &&
      signed_result.ptr == value.data() + value.size()) {
    return signed_value;
  }
  char* float_end = nullptr;
  errno = 0;
  const auto floating_value = std::strtod(value.c_str(), &float_end);
  if (errno == 0 && float_end == value.c_str() + value.size() &&
      float_end != value.c_str() && std::isfinite(floating_value)) {
    return floating_value;
  }
  return value;
}

json yaml_to_json(const YAML::Node& node, int depth, std::size_t& node_count) {
  if (depth > 64) throw std::invalid_argument("profile YAML nesting exceeds 64 levels");
  if (++node_count > 100000) {
    throw std::invalid_argument("profile YAML exceeds 100000 nodes");
  }
  if (!node || node.IsNull()) return nullptr;
  if (node.IsScalar()) return scalar_to_json(node);
  if (node.IsSequence()) {
    auto result = json::array();
    for (const auto& entry : node) {
      result.push_back(yaml_to_json(entry, depth + 1, node_count));
    }
    return result;
  }
  if (node.IsMap()) {
    auto result = json::object();
    std::set<std::string> keys;
    for (const auto& entry : node) {
      if (!entry.first.IsScalar()) {
        throw std::invalid_argument("profile YAML map keys must be strings");
      }
      const auto key = entry.first.Scalar();
      if (!keys.insert(key).second) {
        throw std::invalid_argument("duplicate profile YAML key: " + key);
      }
      result[key] = yaml_to_json(entry.second, depth + 1, node_count);
    }
    return result;
  }
  throw std::invalid_argument("profile YAML contains an unsupported node");
}

YAML::Node json_to_yaml(const json& value) {
  YAML::Node result;
  if (value.is_null()) {
    result = YAML::Node(YAML::NodeType::Null);
  } else if (value.is_boolean()) {
    result = value.get<bool>();
  } else if (value.is_number_integer()) {
    result = value.get<std::int64_t>();
  } else if (value.is_number_unsigned()) {
    result = value.get<std::uint64_t>();
  } else if (value.is_number_float()) {
    result = value.get<double>();
  } else if (value.is_string()) {
    result = value.get<std::string>();
    // A numeric-looking identifier such as accelerator id "0" must survive a
    // YAML round trip as a string, not silently become an integer.
    if (!scalar_to_json(result).is_string()) {
      result.SetTag("tag:yaml.org,2002:str");
    }
  } else if (value.is_array()) {
    result = YAML::Node(YAML::NodeType::Sequence);
    for (const auto& entry : value) result.push_back(json_to_yaml(entry));
  } else if (value.is_object()) {
    result = YAML::Node(YAML::NodeType::Map);
    static const std::vector<std::string> profile_field_order = {
        "schema", "id", "description", "default_chat_model", "available",
        "defaults", "selection", "mode", "memory", "models"};
    std::set<std::string> emitted;
    if (value.contains("schema") && value.contains("id") && value.contains("models")) {
      for (const auto& key : profile_field_order) {
        if (!value.contains(key)) continue;
        result[key] = json_to_yaml(value.at(key));
        emitted.insert(key);
      }
    }
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
      if (emitted.contains(entry.key())) continue;
      result[entry.key()] = json_to_yaml(entry.value());
    }
  } else {
    throw std::invalid_argument("profile document contains an unsupported value");
  }
  return result;
}

json parse_yaml(std::string_view contents, const std::string& source) {
  constexpr std::size_t maximum_profile_bytes = 1024 * 1024;
  if (contents.size() > maximum_profile_bytes) {
    throw std::invalid_argument("profile YAML exceeds 1 MiB: " + source);
  }
  try {
    std::size_t node_count = 0;
    return yaml_to_json(YAML::Load(std::string(contents)), 0, node_count);
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument("invalid profile YAML in " + source + ": " +
                                error.what());
  }
}

void write_yaml_atomic(const std::filesystem::path& path, const json& document) {
  require_yaml_path(path);
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write profile: " + path.string());
  output << profile_yaml(document);
  output.close();
  std::filesystem::rename(temporary, path);
  chmod(path.c_str(), S_IRUSR | S_IWUSR);
}

json read_yaml(const std::filesystem::path& path) {
  require_yaml_path(path);
  if (std::filesystem::is_symlink(path)) {
    throw std::runtime_error("profile files may not be symbolic links: " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read profile: " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  return parse_yaml(contents.str(), path.string());
}

bool external_profile_model(const ModelDefinition& model) {
  return std::find(model.tags.begin(), model.tags.end(), "profile-external") !=
         model.tags.end();
}

void merge_external_model(Registry& registry, ModelDefinition model,
                          const ProfileModel& policy) {
  const auto found = std::find_if(
      registry.models.begin(), registry.models.end(),
      [&](const auto& item) { return item.id == model.id; });
  if (found == registry.models.end()) {
    registry.models.push_back(std::move(model));
    return;
  }
  if (!external_profile_model(*found)) {
    throw std::invalid_argument(
        "known model cannot be redefined as an external model: " + model.id);
  }
  if (found->source_repo != model.source_repo ||
      found->capability != model.capability) {
    throw std::invalid_argument(
        "external model id conflicts with another installed profile: " + model.id);
  }
  const auto incoming_revision = model.repository_revisions.at(policy.backend);
  const auto revision = found->repository_revisions.find(policy.backend);
  if (revision != found->repository_revisions.end() &&
      revision->second != incoming_revision) {
    throw std::invalid_argument(
        "external model backend revision conflicts with another profile: " + model.id);
  }
  auto& artifacts = found->artifacts[policy.backend];
  const auto& incoming = model.artifacts.at(policy.backend).at(policy.quantization);
  const auto artifact = artifacts.find(policy.quantization);
  if (artifact != artifacts.end() &&
      (artifact->second.repository_pattern != incoming.repository_pattern ||
       artifact->second.projector_repository_pattern !=
           incoming.projector_repository_pattern ||
       artifact->second.engine != incoming.engine ||
       artifact->second.format != incoming.format ||
       artifact->second.quantization_type != incoming.quantization_type ||
       artifact->second.sha256 != incoming.sha256 ||
       artifact->second.projector_sha256 != incoming.projector_sha256 ||
       artifact->second.size_bytes != incoming.size_bytes ||
       artifact->second.projector_size_bytes != incoming.projector_size_bytes)) {
    throw std::invalid_argument(
        "external model artifact conflicts with another profile: " + model.id);
  }
  found->repositories[policy.backend] = model.repositories.at(policy.backend);
  found->repository_revisions[policy.backend] = incoming_revision;
  artifacts[policy.quantization] = incoming;
}

bool has_modality(const std::vector<std::string>& modalities,
                  const std::string& modality) {
  return std::find(modalities.begin(), modalities.end(), modality) !=
         modalities.end();
}

bool model_matches_route(const ModelDefinition& model, const std::string& route,
                         const std::vector<std::string>& required_inputs) {
  if (!model.supported_interactions.empty()) {
    const std::string operation = route == "asr" ? "audio.transcribe" :
                                  route == "tts" ? "audio.synthesize_speech" :
                                  route == "diar" ? "audio.diarize" :
                                  route == "completion" ? "text.generate" :
                                  route == "embeddings" ? "embedding.generate" : "chat.generate";
    for (const auto& interaction : model.supported_interactions) {
      if (interaction.operation != operation) continue;
      const auto contains = [](const std::vector<std::string>& values,
                               const std::string& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
      };
      const bool required_present = std::all_of(
          interaction.required_inputs.begin(), interaction.required_inputs.end(),
          [&](const auto& modality) { return contains(required_inputs, modality); });
      const bool request_allowed = std::all_of(
          required_inputs.begin(), required_inputs.end(), [&](const auto& modality) {
            return contains(interaction.required_inputs, modality) ||
                   contains(interaction.optional_inputs, modality);
          });
      if (required_present && request_allowed) return true;
    }
    return false;
  }
  if (route == "asr") {
    return model.capability == "asr" &&
           has_modality(model.input_modalities, "audio") &&
           has_modality(model.output_modalities, "text");
  }
  if (route == "tts") {
    return model.capability == "tts" &&
           has_modality(model.input_modalities, "text") &&
           has_modality(model.output_modalities, "audio");
  }
  if (route != "chat" || !has_modality(model.output_modalities, "text")) {
    return false;
  }
  for (const auto& modality : required_inputs) {
    if (!has_modality(model.input_modalities, modality)) return false;
  }
  return true;
}

bool engine_matches_route(const EngineDefinition& engine, const std::string& route,
                          const std::vector<std::string>& request_inputs) {
  const std::string operation = route == "asr" ? "audio.transcribe" :
                                route == "tts" ? "audio.synthesize_speech" :
                                route == "diar" ? "audio.diarize" :
                                route == "completion" ? "text.generate" :
                                route == "embeddings" ? "embedding.generate" : "chat.generate";
  for (const auto& endpoint : engine.endpoint_contracts) {
    if (endpoint.operation != operation) continue;
    const auto contains = [](const std::vector<std::string>& values,
                             const std::string& value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    };
    const bool required_present = std::all_of(
        endpoint.required_inputs.begin(), endpoint.required_inputs.end(),
        [&](const auto& modality) { return contains(request_inputs, modality); });
    const bool request_allowed = std::all_of(
        request_inputs.begin(), request_inputs.end(), [&](const auto& modality) {
          return contains(endpoint.required_inputs, modality) ||
                 contains(endpoint.optional_inputs, modality);
        });
    if (required_present && request_allowed) return true;
  }
  return false;
}

}  // namespace

void resolve_profile_engines(Registry& registry, Profile& profile,
                            const HardwareInfo& hardware,
                            const std::filesystem::path& root) {
  if (profile.schema < 3) return;
  for (auto& policy : profile.model_policies) {
    if (policy.engine_explicit && policy.artifact_explicit) continue;
    const auto& model = registry.model(policy.id);
    std::vector<std::string> candidates;
    for (const auto& id : model.engine_order) {
      if (policy.engine_explicit && id != policy.engine) continue;
      const auto& engine = registry.engine(id);
      if (!inspect_engine(engine, hardware, root).at("compatible").get<bool>()) continue;
      if (engine.backend == Backend::mlx && !hardware.supports_mlx()) continue;
      if (engine.backend == Backend::gguf && !hardware.supports_gguf()) continue;
      if (engine.backend == Backend::vllm && !hardware.supports_vllm()) continue;
      const auto& variants = model.engine_artifacts.at(id);
      if (policy.artifact_explicit && !variants.contains(policy.quantization)) continue;
      candidates.push_back(id);
    }
    if (candidates.empty()) throw std::invalid_argument("no compatible engine/artifact for " + policy.id);
    const auto installed = [&](const std::string& id) {
      return inspect_engine(registry.engine(id), hardware, root).at("installed").get<bool>();
    };
    if (profile.engine_policy == "prefer-installed") {
      std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        return installed(a) > installed(b);
      });
    }
    policy.engine = candidates.front();
    const auto& engine = registry.engine(policy.engine);
    policy.backend = engine.backend;
    policy.device_target = engine.device_target;
    if (!policy.artifact_explicit) {
      const auto& variants = model.engine_artifacts.at(policy.engine);
      policy.quantization = variants.contains(Quantization::q4) ? Quantization::q4 :
          variants.contains(Quantization::q8) ? Quantization::q8 : variants.begin()->first;
    }
    validate_policy(registry, profile, policy);
  }
  finalize_profile(profile);
}

std::string select_profile_model(const Registry& registry, const Profile& profile,
                                 const std::string& route,
                                 const std::vector<std::string>& required_inputs,
                                 const std::string& requested) {
  const auto eligible = [&](const std::string& id) {
    if (!model_matches_route(registry.model(id), route, required_inputs)) return false;
    const auto* policy = profile.policy_for(id);
    if (!policy) return profile.schema < 5;
    return engine_matches_route(registry.engine(policy->engine), route,
                                required_inputs);
  };
  const auto validate_choice = [&](const std::string& selector) {
    const auto id = selector.substr(0, selector.rfind('@'));
    if (std::find(profile.models.begin(), profile.models.end(), id) ==
        profile.models.end()) {
      throw std::invalid_argument("model is not in active profile: " + id);
    }
    if (!eligible(id)) {
      throw std::invalid_argument("model does not support this request: " + id);
    }
    return selector;
  };
  if (!requested.empty()) return validate_choice(requested);

  std::string default_key = route;
  if (route == "chat") {
    default_key = has_modality(required_inputs, "video") ? "video" :
                  has_modality(required_inputs, "image") ? "image" : "text";
  }
  if (const auto found = profile.default_models.find(default_key);
      found != profile.default_models.end()) {
    return validate_choice(found->second);
  }
  if (default_key == "text" && !profile.default_chat_model.empty()) {
    return validate_choice(profile.default_chat_model);
  }

  std::vector<std::string> candidates;
  for (const auto& id : profile.models) {
    if (eligible(id)) candidates.push_back(id);
  }
  // A text-only chat should prefer text models over multimodal models, but
  // still reject a choice between multiple equally capable text models.
  if (profile.schema < 5 && route == "chat" && default_key == "text") {
    std::vector<std::string> text_candidates;
    for (const auto& id : candidates) {
      if (registry.model(id).capability == "text") text_candidates.push_back(id);
    }
    if (!text_candidates.empty()) candidates = std::move(text_candidates);
  }
  if (candidates.empty()) {
    throw std::invalid_argument("active profile has no model for " + default_key);
  }
  if (candidates.size() > 1) {
    if (profile.schema >= 5) {
      std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        const auto* left = profile.policy_for(a);
        const auto* right = profile.policy_for(b);
        const int left_priority = left ? left->priority : 0;
        const int right_priority = right ? right->priority : 0;
        return left_priority == right_priority ? a < b : left_priority > right_priority;
      });
      return candidates.front();
    }
    throw std::invalid_argument("multiple models support " + default_key +
                                "; specify model or set profile defaults." +
                                default_key);
  }
  return candidates.front();
}

json profile_native_options(const ProfileModel& policy) {
  json result = {{"token_batch_size", policy.token_batch_size},
          {"micro_batch_size", policy.micro_batch_size},
          {"context_checkpoints", policy.context_checkpoints},
          {"ram_cache_mib", policy.ram_cache_mib},
          {"speculative_method", policy.speculative_method},
          {"draft_max_tokens", policy.draft_max_tokens},
          {"draft_gpu_layers", policy.draft_gpu_layers},
          {"draft_kv_cache_precision", policy.draft_kv_cache_precision},
          {"projector_on_cpu", policy.projector_on_cpu}};
  if (!policy.kv_cache_k_precision.empty()) result["kv_cache_k_precision"] = policy.kv_cache_k_precision;
  if (!policy.kv_cache_v_precision.empty()) result["kv_cache_v_precision"] = policy.kv_cache_v_precision;
  return result;
}

json profile_to_document(const Profile& profile) {
  json models = json::array();
  for (const auto& policy : profile.model_policies) {
    json placement = {{"mode", policy.placement_mode}, {"device", policy.device}};
    if (policy.gpu_layers >= 0) placement["gpu_layers"] = policy.gpu_layers;
    if (policy.ram_reservation_gib >= 0) {
      placement["ram_reservation_gib"] = policy.ram_reservation_gib;
    }
    if (policy.vram_reservation_gib >= 0) {
      placement["vram_reservation_gib"] = policy.vram_reservation_gib;
    }
    json item = {{"id", policy.id},
                 {"placement", placement},
                 {"residency", to_string(policy.residency)},
                 {"priority", policy.priority},
                 {"startup", policy.startup},
                 {"idle_seconds", policy.idle_seconds},
                 {"context", {{"max_input_tokens", policy.max_input_tokens},
                              {"max_output_tokens", policy.max_output_tokens},
                              {"max_total_tokens", policy.max_total_tokens}}},
                 {"batching", {{"max_concurrent_requests",
                                 policy.max_concurrent_requests}}},
                 {"kv_cache", {{"precision", policy.kv_cache_precision}}}};
    if (policy.engine_explicit) item["engine"] = policy.engine;
    if (policy.artifact_explicit) item["artifact"] = {{"id", to_string(policy.quantization)}};
    if (profile.schema >= 5) {
      if (!policy.kv_cache_k_precision.empty()) item["kv_cache"]["key_precision"] = policy.kv_cache_k_precision;
      if (!policy.kv_cache_v_precision.empty()) item["kv_cache"]["value_precision"] = policy.kv_cache_v_precision;
      if (policy.token_batch_size > 0) item["batching"]["token_batch_size"] = policy.token_batch_size;
      if (policy.micro_batch_size > 0) item["batching"]["micro_batch_size"] = policy.micro_batch_size;
      if (policy.context_checkpoints >= 0) item["kv_cache"]["context_checkpoints"] = policy.context_checkpoints;
      if (policy.ram_cache_mib >= 0) item["kv_cache"]["ram_cache_mib"] = policy.ram_cache_mib;
      if (policy.projector_on_cpu) item["placement"]["projector_on_cpu"] = true;
      if (policy.speculative_method != "auto" || policy.draft_gpu_layers >= 0 ||
          policy.draft_max_tokens != 7 || policy.draft_kv_cache_precision != "q4") {
        item["speculative"] = {{"method", policy.speculative_method},
                                {"max_draft_tokens", policy.draft_max_tokens},
                                {"kv_cache_precision", policy.draft_kv_cache_precision}};
        if (policy.draft_gpu_layers >= 0) item["speculative"]["gpu_layers"] = policy.draft_gpu_layers;
      }
    }
    models.push_back(std::move(item));
  }
  json document = {{"schema", profile.schema >= 5 ? 5 : 4},
          {"id", profile.name},
          {"description", profile.description},
          {"mode", profile.mode},
          {"catalog_visible", profile.catalog_visible},
          {"memory", {{"required_ram_gib", profile.required_ram_gib},
                      {"required_vram_gib", profile.required_vram_gib},
                      {"safety_reserve_gib", profile.memory_safety_reserve_gib},
                      {"maximum_resident_workers", profile.maximum_resident_workers}}},
          {"models", models}};
  if (profile.schema >= 5) {
    if (profile.memory_limit_gib > 0) document["memory"]["limit_gib"] = profile.memory_limit_gib;
    if (!profile.residency_strategy.empty()) {
      document["memory"]["strategy"] = profile.residency_strategy;
      document["memory"]["keep_resident"] = profile.balanced_keep_models;
    }
    json defaults = json::array();
    for (const auto& [category, id] : profile.default_models) {
      const std::string operation = category == "asr" ? "audio.transcribe" :
                                    category == "tts" ? "audio.synthesize_speech" :
                                    category == "diar" ? "audio.diarize" :
                                    category == "completion" ? "text.generate" :
                                    category == "embeddings" ? "embedding.generate" : "chat.generate";
      const std::vector<std::string> inputs = category == "asr" || category == "diar" ?
          std::vector<std::string>{"audio"} : category == "image" ?
          std::vector<std::string>{"text", "image"} : category == "video" ?
          std::vector<std::string>{"text", "video"} :
          std::vector<std::string>{"text"};
      defaults.push_back({{"operation", operation}, {"required_inputs", inputs},
                          {"model", id}});
    }
    if (defaults.empty() && !profile.default_chat_model.empty()) {
      defaults.push_back({{"operation", "chat.generate"},
                          {"required_inputs", {"text"}},
                          {"model", profile.default_chat_model}});
    }
    document["selection"] = {{"engine_policy", profile.engine_policy}, {"defaults", defaults}};
  } else {
    document["default_chat_model"] = profile.default_chat_model;
    if (!profile.default_models.empty()) document["defaults"] = profile.default_models;
  }
  return document;
}

std::string profile_yaml(const json& document) {
  YAML::Emitter output;
  output.SetIndent(2);
  output << json_to_yaml(document);
  if (!output.good()) {
    throw std::runtime_error("cannot serialize profile YAML: " +
                             output.GetLastError());
  }
  return std::string(output.c_str()) + "\n";
}

Profile profile_from_document(Registry& registry, const json& document) {
  if (!document.is_object() ||
      (document.value("schema", 0) != 3 && document.value("schema", 0) != 4 &&
       document.value("schema", 0) != 5)) {
    throw std::invalid_argument("profile file must use schema 3, 4, or 5");
  }
  reject_unknown_fields(document,
        {"schema", "id", "description", "available", "mode",
         "default_chat_model", "defaults", "selection", "catalog_visible",
         "memory", "models"},
        "profile");
  Registry working = registry;
  Profile profile;
  profile.schema = document.at("schema").get<int>();
  profile.name = document.at("id").get<std::string>();
  profile.description = document.value("description", std::string());
  if (profile.schema >= 5 && profile.description.empty()) {
    throw std::invalid_argument("schema-5 workload profile needs an intended-use description");
  }
  profile.catalog_visible = document.value("catalog_visible", true);
  profile.default_chat_model = document.value("default_chat_model", std::string());
  if (profile.schema >= 5 &&
      (document.contains("default_chat_model") || document.contains("defaults"))) {
    throw std::invalid_argument("schema-5 workload selection belongs under selection.defaults");
  }
  if (document.contains("selection")) {
    const auto& selection = document.at("selection");
    reject_unknown_fields(selection, {"engine_policy", "defaults"}, "profile selection");
    const auto policy = selection.value("engine_policy", std::string("prefer-installed"));
    if (policy != "explicit-only" && policy != "prefer-installed" && policy != "manifest-order") {
      throw std::invalid_argument("unknown engine selection policy");
    }
    profile.engine_policy = policy;
    for (const auto& entry : selection.value("defaults", json::array())) {
      reject_unknown_fields(entry, {"operation", "required_inputs", "model"},
                            "profile selection default");
      const auto operation = entry.at("operation").get<std::string>();
      const auto inputs = entry.at("required_inputs").get<std::vector<std::string>>();
      std::string category;
      if (operation == "audio.transcribe" && inputs == std::vector<std::string>{"audio"}) {
        category = "asr";
      } else if (operation == "audio.diarize" && inputs == std::vector<std::string>{"audio"}) {
        category = "diar";
      } else if (operation == "text.generate" && inputs == std::vector<std::string>{"text"}) {
        category = "completion";
      } else if (operation == "embedding.generate" && inputs == std::vector<std::string>{"text"}) {
        category = "embeddings";
      } else if (operation == "audio.synthesize_speech" &&
                 inputs == std::vector<std::string>{"text"}) {
        category = "tts";
      } else if (operation == "chat.generate") {
        const std::set<std::string> input_set(inputs.begin(), inputs.end());
        if (input_set == std::set<std::string>{"text"}) category = "text";
        if (input_set == std::set<std::string>{"text", "image"}) category = "image";
        if (input_set == std::set<std::string>{"text", "video"}) category = "video";
      }
      if (category.empty() || profile.default_models.contains(category)) {
        throw std::invalid_argument("unsupported or duplicate profile selection default");
      }
      profile.default_models[category] = entry.at("model").get<std::string>();
    }
  }
  if (profile.schema >= 5 && profile.default_models.contains("text")) {
    // Keep the existing public API and playground default in sync with the
    // operation-based selection without restoring a legacy YAML field.
    profile.default_chat_model = profile.default_models.at("text");
  }
  if (document.contains("defaults")) {
    const auto& defaults = document.at("defaults");
    reject_unknown_fields(defaults, {"text", "image", "video", "asr", "tts"},
                          "profile defaults");
    for (auto item = defaults.begin(); item != defaults.end(); ++item) {
      if (!item.value().is_string()) {
        throw std::invalid_argument("profile default must be a model id: " + item.key());
      }
      profile.default_models[item.key()] = item.value().get<std::string>();
    }
  }
  if (profile.default_models.contains("text") &&
      !profile.default_chat_model.empty() &&
      profile.default_models.at("text") != profile.default_chat_model) {
    throw std::invalid_argument("defaults.text conflicts with default_chat_model");
  }
  if (!safe_id(profile.name)) throw std::invalid_argument("profile id is invalid");
  profile.mode = document.value("mode", std::string("interactive"));
  const auto memory = document.value("memory", json::object());
  reject_unknown_fields(memory,
                        {"required_ram_gib", "required_vram_gib",
                         "safety_reserve_gib", "maximum_resident_workers",
                         "limit_gib", "strategy", "keep_resident",
                         "maximum_ram_gib", "maximum_vram_gib"},
                        "profile memory");
  if (profile.schema >= 4 &&
      (memory.contains("maximum_ram_gib") || memory.contains("maximum_vram_gib"))) {
    throw std::invalid_argument(
        "schema-4 workload memory declares requirements, not maximum usage");
  }
  if (profile.schema >= 4 && !memory.contains("strategy") &&
      (!memory.contains("required_ram_gib") ||
       !memory.contains("required_vram_gib"))) {
    throw std::invalid_argument(
        "schema-4 workload memory requires RAM and VRAM requirements");
  }
  profile.required_ram_gib = memory.value("required_ram_gib", 0.0);
  profile.required_vram_gib = memory.value("required_vram_gib", 0.0);
  if (profile.schema == 3 &&
      (memory.contains("maximum_ram_gib") || memory.contains("maximum_vram_gib"))) {
    profile.required_ram_gib = memory.value("maximum_ram_gib", 0.0);
    profile.required_vram_gib = memory.value("maximum_vram_gib", 0.0);
  }
  profile.memory_safety_reserve_gib = memory.value("safety_reserve_gib", 0.5);
  profile.maximum_resident_workers = memory.value("maximum_resident_workers", 0);
  profile.memory_limit_gib = memory.value("limit_gib", 0.0);
  profile.residency_strategy = memory.value("strategy", std::string());
  profile.balanced_keep_models = memory.value("keep_resident", std::vector<std::string>{});
  if (!std::isfinite(profile.memory_limit_gib) || profile.memory_limit_gib < 0 ||
      (memory.contains("limit_gib") && profile.memory_limit_gib == 0))
    throw std::invalid_argument("memory.limit_gib must be positive");
  if (!document.contains("models") || !document.at("models").is_array() ||
      document.at("models").empty() || document.at("models").size() > 128) {
    throw std::invalid_argument("profile must contain a models array");
  }
  std::set<std::string> ids;
  for (const auto& entry : document.at("models")) {
    if (!entry.is_object()) throw std::invalid_argument("profile model must be an object");
    reject_unknown_fields(
        entry,
        {"id", "execution", "description", "tags", "modality", "engine",
         "family", "declared_context_tokens", "source", "artifact", "context",
         "batching", "kv_cache", "placement", "residency", "priority",
         "startup", "idle_seconds", "speculative"},
        "profile model");
    ProfileModel policy;
    policy.id = entry.at("id").get<std::string>();
    if (!safe_id(policy.id) || !ids.insert(policy.id).second) {
      throw std::invalid_argument("invalid or duplicate profile model id: " + policy.id);
    }
    policy.execution = entry.value("execution", std::string());
    if (profile.schema >= 4 && !policy.execution.empty()) {
      throw std::invalid_argument("schema-4-or-newer workloads cannot use execution templates");
    }
    const auto existing = std::find_if(
        working.models.begin(), working.models.end(),
        [&](const auto& model) { return model.id == policy.id; });
    if (!policy.execution.empty()) {
      if (entry.contains("source") || entry.contains("artifact") ||
          entry.contains("modality") || entry.contains("engine")) {
        throw std::invalid_argument(
            "profile model cannot combine execution with an external model definition");
      }
      policy = execution_template(working, policy.execution);
      if (policy.id != entry.at("id").get<std::string>()) {
        throw std::invalid_argument("execution profile model does not match entry id");
      }
    } else if (existing != working.models.end() && !entry.contains("source")) {
      if (entry.contains("modality") || entry.contains("family") ||
          entry.contains("declared_context_tokens")) {
        throw std::invalid_argument(
            "known model profile cannot redefine catalog metadata: " + policy.id);
      }
      policy.engine_explicit = entry.contains("engine");
      policy.artifact_explicit = entry.contains("artifact");
      if (profile.engine_policy == "explicit-only" &&
          (!policy.engine_explicit || !policy.artifact_explicit)) {
        throw std::invalid_argument(
            "known model requires an execution or explicit engine and artifact: " +
            policy.id);
      }
      if (existing->engine_order.empty()) throw std::invalid_argument("model has no engines: " + policy.id);
      policy.engine = entry.value("engine", existing->engine_order.front());
      const auto& engine = working.engine(policy.engine);
      if (engine.status != "current" && engine.status != "candidate") {
        throw std::invalid_argument("profile engine is not runnable: " + policy.engine);
      }
      policy.backend = engine.backend;
      policy.device_target = engine.device_target;
      const auto selected = entry.value("artifact", json{{"id", "q4"}});
      reject_unknown_fields(selected, {"id", "format"}, "known model artifact");
      policy.quantization = parse_quantization(selected.at("id").get<std::string>());
      if (!policy.artifact_explicit) {
        const auto& variants = existing->engine_artifacts.at(policy.engine);
        if (!variants.contains(policy.quantization)) policy.quantization = variants.begin()->first;
      }
      const auto& artifact = existing->artifact_for(policy.backend, policy.quantization, policy.engine);
      if (artifact.engine != policy.engine ||
          (selected.contains("format") &&
           selected.at("format").get<std::string>() != artifact.format)) {
        throw std::invalid_argument("artifact and engine do not match for " + policy.id);
      }
    } else {
      policy.id = entry.at("id").get<std::string>();
      policy.execution = profile.name + ":" + policy.id;
      auto model = external_model(working, entry, policy);
      if (existing != working.models.end() && !external_profile_model(*existing)) {
        throw std::invalid_argument(
            "known model cannot be redefined as an external model: " + policy.id);
      }
      merge_external_model(working, std::move(model), policy);
    }
    policy.residency = parse_residency(
        entry.value("residency", std::string("on-demand")));
    policy.priority = entry.value("priority", policy.priority);
    policy.startup = entry.value("startup", policy.startup);
    policy.idle_seconds = entry.value(
        "idle_seconds", policy.residency == Residency::pinned ? 0 :
                        policy.residency == Residency::ephemeral ? 0 : 300);
    if (entry.contains("context")) {
      const auto& context = entry.at("context");
      reject_unknown_fields(context,
                            {"max_input_tokens", "max_output_tokens",
                             "max_total_tokens"},
                            "profile model context");
      policy.max_input_tokens = context.value("max_input_tokens", 2048);
      policy.max_output_tokens = context.value("max_output_tokens", 256);
      policy.max_total_tokens = context.value(
          "max_total_tokens", policy.max_input_tokens + policy.max_output_tokens);
    }
    if (entry.contains("batching")) {
      reject_unknown_fields(entry.at("batching"),
                            {"max_concurrent_requests", "token_batch_size", "micro_batch_size"},
                            "profile model batching");
      policy.max_concurrent_requests =
          entry.at("batching").value("max_concurrent_requests", 1);
      for (const auto key : {"token_batch_size", "micro_batch_size"}) {
        if (entry.at("batching").contains(key) &&
            !entry.at("batching").at(key).is_number_integer())
          throw std::invalid_argument("native batch sizes must be integers");
      }
      policy.token_batch_size = entry.at("batching").value("token_batch_size", 0);
      policy.micro_batch_size = entry.at("batching").value("micro_batch_size", 0);
    }
    if (entry.contains("kv_cache")) {
      reject_unknown_fields(entry.at("kv_cache"), {"precision", "key_precision", "value_precision", "context_checkpoints", "ram_cache_mib"},
                            "profile model KV cache");
      policy.kv_cache_precision =
          entry.at("kv_cache").value("precision", std::string("q8"));
      policy.kv_cache_k_precision = entry.at("kv_cache").value("key_precision", std::string());
      policy.kv_cache_v_precision = entry.at("kv_cache").value("value_precision", std::string());
      for (const auto key : {"context_checkpoints", "ram_cache_mib"}) {
        if (entry.at("kv_cache").contains(key) &&
            !entry.at("kv_cache").at(key).is_number_integer())
          throw std::invalid_argument("native cache sizes must be integers");
      }
      policy.context_checkpoints = entry.at("kv_cache").value("context_checkpoints", -1);
      policy.ram_cache_mib = entry.at("kv_cache").value("ram_cache_mib", -1);
    }
    if (entry.contains("speculative")) {
      const auto& speculative = entry.at("speculative");
      reject_unknown_fields(speculative, {"method", "max_draft_tokens", "gpu_layers", "kv_cache_precision"},
                            "profile speculative decoding");
      for (const auto key : {"max_draft_tokens", "gpu_layers"}) {
        if (speculative.contains(key) && !speculative.at(key).is_number_integer())
          throw std::invalid_argument("speculative sizes must be integers");
      }
      policy.speculative_method = speculative.value("method", std::string("auto"));
      policy.draft_max_tokens = speculative.value("max_draft_tokens", 7);
      policy.draft_gpu_layers = speculative.value("gpu_layers", -1);
      policy.draft_kv_cache_precision = speculative.value("kv_cache_precision", std::string("q4"));
    }
    if (entry.contains("placement")) {
      const auto& placement = entry.at("placement");
      if (!placement.is_object()) {
        throw std::invalid_argument("placement must be an object for " + policy.id);
      }
      reject_unknown_fields(
          placement,
          {"mode", "device", "gpu_layers", "ram_reservation_gib",
           "vram_reservation_gib", "projector_on_cpu"},
          "profile model placement");
      policy.placement_mode = placement.value("mode", std::string("auto"));
      policy.device = placement.value("device", std::string("auto"));
      policy.gpu_layers = placement.value("gpu_layers", -1);
      policy.ram_reservation_gib = placement.value("ram_reservation_gib", -1.0);
      policy.vram_reservation_gib = placement.value("vram_reservation_gib", -1.0);
      policy.projector_on_cpu = placement.value("projector_on_cpu", false);
    }
    if (policy.engine_explicit && policy.artifact_explicit) validate_policy(working, profile, policy);
    profile.models.push_back(policy.id);
    profile.model_policies.push_back(std::move(policy));
  }
  if (!memory.contains("keep_resident")) for (const auto& policy : profile.model_policies)
    if (policy.residency == Residency::pinned) profile.balanced_keep_models.push_back(policy.id);
  std::set<std::string> keep_ids;
  for (const auto& id : profile.balanced_keep_models)
    if (!ids.contains(id) || !keep_ids.insert(id).second)
      throw std::invalid_argument("keep_resident must contain unique workload model IDs");
  apply_residency_strategy(profile);
  if (std::any_of(profile.model_policies.begin(), profile.model_policies.end(),
      [](const auto& p) { return !p.engine_explicit || !p.artifact_explicit; })) {
    const auto hardware = registry.resolution_hardware ? *registry.resolution_hardware : detect_hardware();
    auto root = registry.runtime_root;
    if (root.empty()) {
      if (const auto* home = std::getenv("MICA_HOME")) root = home;
      else if (const auto* home = std::getenv("HOME")) root = std::string(home) + "/.mica";
    }
    resolve_profile_engines(working, profile, hardware, root);
  }
  finalize_profile(profile);
  if (!profile.default_chat_model.empty()) {
    if (std::find(profile.models.begin(), profile.models.end(),
                  profile.default_chat_model) == profile.models.end()) {
      throw std::invalid_argument("default_chat_model is not in the profile: " +
                                  profile.default_chat_model);
    }
    const auto& default_model = working.model(profile.default_chat_model);
    if (default_model.capability != "text" && default_model.capability != "vision") {
      throw std::invalid_argument("default_chat_model must generate text");
    }
  }
  for (const auto& [category, id] : profile.default_models) {
    const auto inputs = category == "asr" || category == "diar" ? std::vector<std::string>{"audio"} :
                        category == "image" ? std::vector<std::string>{"text", "image"} :
                        category == "video" ? std::vector<std::string>{"text", "video"} :
                        std::vector<std::string>{"text"};
    const auto route = category == "asr" || category == "tts" || category == "diar" || category == "completion" || category == "embeddings" ? category : "chat";
    try {
      (void)select_profile_model(working, profile, route, inputs, id);
    } catch (const std::invalid_argument& error) {
      throw std::invalid_argument("invalid profile default " + category + ": " +
                                  error.what());
    }
  }
  registry.models = std::move(working.models);
  return profile;
}

json read_profile_file(const std::filesystem::path& path) {
  return read_yaml(path);
}

void write_profile_file(const std::filesystem::path& path, const json& document) {
  write_yaml_atomic(path, document);
}

std::string merge_profile_file(Registry& registry, const std::filesystem::path& path) {
  auto profile = profile_from_document(registry, read_yaml(path));
  const auto name = profile.name;
  registry.profiles[profile.name] = std::move(profile);
  return name;
}

void merge_installed_profiles(Registry& registry, const std::filesystem::path& root) {
  const auto directory = root / "config/profiles";
  if (!std::filesystem::is_directory(directory)) return;
  std::vector<std::filesystem::path> files;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().extension() == ".yaml" || item.path().extension() == ".yml") {
      files.push_back(item.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& path : files) merge_profile_file(registry, path);
}

std::filesystem::path install_profile_file(Registry& registry,
                                           const std::filesystem::path& root,
                                           const std::filesystem::path& source) {
  const auto document = read_yaml(source);
  const auto profile = profile_from_document(registry, document);
  const auto destination = root / "config/profiles" /
                           (profile.name + ".yaml");
  write_yaml_atomic(destination, document);
  return destination;
}

json fetch_profile_catalog(const std::string& url) {
  if (url.rfind("https://raw.githubusercontent.com/", 0) != 0) {
    throw std::invalid_argument("profile catalog URL must use raw.githubusercontent.com HTTPS");
  }
  const auto result = run_command(
      {"curl", "--location", "--fail", "--silent", "--show-error",
       "--max-time", "20", url},
      true);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot download profile catalog: " + result.output);
  }
  auto catalog = parse_yaml(result.output, url);
  if (catalog.value("schema", 0) != 1 || !catalog.contains("profiles") ||
      !catalog.at("profiles").is_array()) {
    throw std::runtime_error("remote profile catalog has an unsupported schema");
  }
  return catalog;
}

std::filesystem::path install_profile_from_catalog(Registry& registry,
                                                   const std::filesystem::path& root,
                                                   const std::string& id,
                                                   const std::string& url) {
  const auto catalog = fetch_profile_catalog(url);
  const auto found = std::find_if(
      catalog.at("profiles").begin(), catalog.at("profiles").end(),
      [&](const auto& document) { return document.value("id", "") == id; });
  if (found == catalog.at("profiles").end()) {
    throw std::out_of_range("remote profile does not exist: " + id);
  }
  if (!found->value("available", true)) {
    throw std::runtime_error("remote profile is not installable: " + id +
                             ": " + found->value("reason", "not available"));
  }
  auto document = *found;
  document.erase("reason");
  document.erase("status");
  const auto profile = profile_from_document(registry, document);
  const auto destination = root / "config/profiles" /
                           (profile.name + ".yaml");
  write_yaml_atomic(destination, document);
  return destination;
}

}  // namespace mica

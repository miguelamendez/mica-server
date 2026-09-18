#include "mica_server/profiles.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include "mica_server/catalog.hpp"
#include "mica_server/command.hpp"

namespace mica {
namespace {

using json = nlohmann::json;

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
      "isc", "cc0-1.0", "unlicense", "mpl-2.0"};
  auto normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return allowed.contains(normalized);
}

Backend backend_for_engine(const std::string& engine) {
  if (engine == "mlx-lm" || engine == "mlx-vlm" || engine == "mlx-audio") {
    return Backend::mlx;
  }
  if (engine == "llama-cpp" || engine == "audio-cpp") return Backend::gguf;
  if (engine == "vllm") return Backend::vllm;
  throw std::invalid_argument("unsupported profile engine: " + engine);
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
  const auto artifact = backend->second.find(policy.quantization);
  if (artifact == backend->second.end() || !artifact->second.supported) {
    throw std::invalid_argument("model has no selected artifact: " + policy.id + "@" +
                                to_string(policy.backend) + ":" +
                                to_string(policy.quantization));
  }
  if (policy.max_input_tokens < 1 || policy.max_output_tokens < 1 ||
      policy.max_total_tokens < policy.max_input_tokens + policy.max_output_tokens) {
    throw std::invalid_argument("inconsistent context limits for " + policy.id);
  }
  if ((model.capability == "text" || model.capability == "vision") &&
      policy.max_total_tokens > model.gguf_context_tokens) {
    throw std::invalid_argument("profile exceeds declared context for " + policy.id);
  }
  if (policy.max_concurrent_requests < 1 || policy.max_concurrent_requests > 64) {
    throw std::invalid_argument("invalid concurrency for " + policy.id);
  }
  if (policy.priority < 0 || policy.priority > 1000 || policy.idle_seconds < 0) {
    throw std::invalid_argument("invalid residency policy for " + policy.id);
  }
  if (profile.maximum_ram_gib < 0 || profile.maximum_vram_gib < 0 ||
      profile.memory_safety_reserve_gib < 0 || profile.maximum_resident_workers < 0) {
    throw std::invalid_argument("profile memory values cannot be negative");
  }
}

double conservative_reservation(const json& artifact, Quantization quantization) {
  const double size = artifact.value("size_gib", 0.0);
  if (size > 0) return std::max(1.0, size * 1.4 + 0.5);
  return quantization == Quantization::q4 ? 4.0 :
         quantization == Quantization::q8 ? 8.0 : 12.0;
}

ModelDefinition external_model(const json& entry, ProfileModel& policy) {
  if (!entry.contains("source") || !entry.at("source").is_object()) {
    throw std::invalid_argument("external model requires a source object");
  }
  const auto& source = entry.at("source");
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
  policy.backend = backend_for_engine(policy.engine);
  const auto& artifact_document = entry.at("artifact");
  const auto expected_format = format_for_backend(policy.backend);
  const auto format = artifact_document.value("format", expected_format);
  if ((policy.backend == Backend::gguf && format != "gguf") ||
      (policy.backend == Backend::mlx && format != "mlx") ||
      (policy.backend == Backend::vllm && format != "safetensors" &&
       format != "compressed-tensors")) {
    throw std::invalid_argument("artifact format does not match the selected engine");
  }
  policy.quantization = parse_quantization(
      artifact_document.value("quantization", std::string("q4")));
  Artifact artifact;
  artifact.supported = true;
  artifact.pattern = artifact_document.at("path").get<std::string>();
  artifact.repository_pattern = artifact.pattern;
  artifact.projector_pattern = artifact_document.value("projector", std::string());
  artifact.projector_repository_pattern = artifact.projector_pattern;
  artifact.reservation_gib = artifact_document.value(
      "reservation_gib", conservative_reservation(artifact_document,
                                                    policy.quantization));
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
  if (policy.engine == "audio-cpp") {
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

void write_json_atomic(const std::filesystem::path& path, const json& document) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write profile: " + path.string());
  output << std::setw(2) << document << '\n';
  output.close();
  std::filesystem::rename(temporary, path);
  chmod(path.c_str(), S_IRUSR | S_IWUSR);
}

json read_json(const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(path)) {
    throw std::runtime_error("profile files may not be symbolic links: " + path.string());
  }
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read profile: " + path.string());
  return json::parse(input);
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
        "known model requires an execution profile reference: " + model.id);
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
           incoming.projector_repository_pattern)) {
    throw std::invalid_argument(
        "external model artifact conflicts with another profile: " + model.id);
  }
  found->repositories[policy.backend] = model.repositories.at(policy.backend);
  found->repository_revisions[policy.backend] = incoming_revision;
  artifacts[policy.quantization] = incoming;
}

}  // namespace

json profile_to_json(const Profile& profile) {
  json models = json::array();
  for (const auto& policy : profile.model_policies) {
    models.push_back({{"id", policy.id},
                      {"execution", policy.execution},
                      {"residency", to_string(policy.residency)},
                      {"priority", policy.priority},
                      {"startup", policy.startup},
                      {"idle_seconds", policy.idle_seconds}});
  }
  return {{"schema", 2},
          {"id", profile.name},
          {"mode", profile.mode},
          {"memory", {{"maximum_ram_gib", profile.maximum_ram_gib},
                      {"maximum_vram_gib", profile.maximum_vram_gib},
                      {"safety_reserve_gib", profile.memory_safety_reserve_gib},
                      {"maximum_resident_workers", profile.maximum_resident_workers}}},
          {"models", models}};
}

Profile profile_from_json(Registry& registry, const json& document) {
  if (!document.is_object() || document.value("schema", 0) != 2) {
    throw std::invalid_argument("profile file must use schema 2");
  }
  Registry working = registry;
  Profile profile;
  profile.schema = 2;
  profile.name = document.at("id").get<std::string>();
  if (!safe_id(profile.name)) throw std::invalid_argument("profile id is invalid");
  profile.mode = document.value("mode", std::string("interactive"));
  const auto memory = document.value("memory", json::object());
  profile.maximum_ram_gib = memory.value("maximum_ram_gib", 8.0);
  profile.maximum_vram_gib = memory.value("maximum_vram_gib", 0.0);
  profile.memory_safety_reserve_gib = memory.value("safety_reserve_gib", 0.5);
  profile.maximum_resident_workers = memory.value("maximum_resident_workers", 0);
  if (!document.contains("models") || !document.at("models").is_array() ||
      document.at("models").empty()) {
    throw std::invalid_argument("profile must contain a models array");
  }
  std::set<std::string> ids;
  for (const auto& entry : document.at("models")) {
    if (!entry.is_object()) throw std::invalid_argument("profile model must be an object");
    ProfileModel policy;
    policy.id = entry.at("id").get<std::string>();
    if (!safe_id(policy.id) || !ids.insert(policy.id).second) {
      throw std::invalid_argument("invalid or duplicate profile model id: " + policy.id);
    }
    policy.execution = entry.value("execution", std::string());
    const auto existing = std::find_if(
        working.models.begin(), working.models.end(),
        [&](const auto& model) { return model.id == policy.id; });
    if (!policy.execution.empty()) {
      policy = execution_template(working, policy.execution);
      if (policy.id != entry.at("id").get<std::string>()) {
        throw std::invalid_argument("execution profile model does not match entry id");
      }
    } else {
      policy.id = entry.at("id").get<std::string>();
      policy.execution = profile.name + ":" + policy.id;
      auto model = external_model(entry, policy);
      if (existing != working.models.end() && !external_profile_model(*existing)) {
        throw std::invalid_argument(
            "known model requires an execution profile reference: " + policy.id);
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
      policy.max_input_tokens = context.value("max_input_tokens", 2048);
      policy.max_output_tokens = context.value("max_output_tokens", 256);
      policy.max_total_tokens = context.value(
          "max_total_tokens", policy.max_input_tokens + policy.max_output_tokens);
    }
    if (entry.contains("batching")) {
      policy.max_concurrent_requests =
          entry.at("batching").value("max_concurrent_requests", 1);
    }
    if (entry.contains("kv_cache")) {
      policy.kv_cache_precision =
          entry.at("kv_cache").value("precision", std::string("q8"));
    }
    validate_policy(working, profile, policy);
    profile.models.push_back(policy.id);
    profile.model_policies.push_back(std::move(policy));
  }
  finalize_profile(profile);
  registry.models = std::move(working.models);
  return profile;
}

json read_profile_file(const std::filesystem::path& path) {
  return read_json(path);
}

void write_profile_file(const std::filesystem::path& path, const json& document) {
  write_json_atomic(path, document);
}

std::string merge_profile_file(Registry& registry, const std::filesystem::path& path) {
  auto profile = profile_from_json(registry, read_json(path));
  const auto name = profile.name;
  registry.profiles[profile.name] = std::move(profile);
  return name;
}

void merge_installed_profiles(Registry& registry, const std::filesystem::path& root) {
  const auto directory = root / "mica-server/profiles";
  if (!std::filesystem::is_directory(directory)) return;
  std::vector<std::filesystem::path> files;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().extension() == ".json") files.push_back(item.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& path : files) merge_profile_file(registry, path);
}

std::filesystem::path install_profile_file(Registry& registry,
                                           const std::filesystem::path& root,
                                           const std::filesystem::path& source) {
  const auto document = read_json(source);
  const auto profile = profile_from_json(registry, document);
  const auto destination = root / "mica-server/profiles" /
                           (profile.name + ".json");
  write_json_atomic(destination, document);
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
  auto catalog = json::parse(result.output);
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
  const auto profile = profile_from_json(registry, *found);
  const auto destination = root / "mica-server/profiles" /
                           (profile.name + ".json");
  write_json_atomic(destination, *found);
  return destination;
}

}  // namespace mica

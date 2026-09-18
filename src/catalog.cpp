#include "mica_server/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "mica_server/command.hpp"
#include "mica_server/config.hpp"

namespace mica {
namespace {

using json = nlohmann::json;

std::filesystem::path catalog_path(const std::filesystem::path& root) {
  return root / "mica-server/custom-models.json";
}

json read_catalog(const std::filesystem::path& root) {
  const auto path = catalog_path(root);
  if (!std::filesystem::exists(path)) {
    return {{"schema", 1}, {"models", json::object()}};
  }
  std::ifstream file(path);
  auto catalog = json::parse(file);
  if (!catalog.contains("models") || !catalog["models"].is_object()) {
    throw std::runtime_error("custom-models.json has no models object");
  }
  return catalog;
}

void write_catalog(const std::filesystem::path& root, const json& catalog) {
  const auto path = catalog_path(root);
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream file(temporary, std::ios::trunc);
  file << std::setw(2) << catalog << '\n';
  file.close();
  std::filesystem::rename(temporary, path);
}

void run_or_print(const std::vector<std::string>& command, bool dry_run) {
  std::cout << (dry_run ? "[plan] " : "[run]  ") << display_command(command) << '\n';
  if (dry_run) return;
  const auto result = run_command(command, true);
  if (result.exit_code != 0) {
    throw std::runtime_error("command failed: " + display_command(command) + "\n" +
                             result.output);
  }
}

bool safe_repo_character(char value) {
  const auto character = static_cast<unsigned char>(value);
  return std::isalnum(character) || value == '-' || value == '_' || value == '.';
}

std::string hugging_face_repo(const std::string& input) {
  std::string value = input;
  constexpr const char* prefix = "https://huggingface.co/";
  if (value.rfind(prefix, 0) == 0) value.erase(0, std::char_traits<char>::length(prefix));
  while (!value.empty() && value.back() == '/') value.pop_back();
  const auto query = value.find_first_of("?#");
  if (query != std::string::npos) value.erase(query);
  const auto first = value.find('/');
  if (first == std::string::npos || first == 0) {
    throw std::invalid_argument("Hugging Face URL must identify OWNER/REPO");
  }
  const auto second = value.find('/', first + 1);
  if (second != std::string::npos) value.erase(second);
  const auto owner = value.substr(0, first);
  const auto repo = value.substr(first + 1);
  if (repo.empty() || !std::all_of(owner.begin(), owner.end(), safe_repo_character) ||
      !std::all_of(repo.begin(), repo.end(), safe_repo_character)) {
    throw std::invalid_argument("Hugging Face owner/repository contains invalid characters");
  }
  return owner + "/" + repo;
}

std::string slug_from_repo(const std::string& repo) {
  auto slug = repo.substr(repo.find('/') + 1);
  std::transform(slug.begin(), slug.end(), slug.begin(), [](unsigned char value) {
    if (std::isalnum(value)) return static_cast<char>(std::tolower(value));
    return '-';
  });
  std::string compact;
  for (const auto value : slug) {
    if (value != '-' || compact.empty() || compact.back() != '-') compact.push_back(value);
  }
  while (!compact.empty() && compact.back() == '-') compact.pop_back();
  return compact;
}

std::string model_license(const json& metadata) {
  if (metadata.contains("cardData") && metadata["cardData"].is_object() &&
      metadata["cardData"].contains("license") && metadata["cardData"]["license"].is_string()) {
    return metadata["cardData"]["license"].get<std::string>();
  }
  for (const auto& tag : metadata.value("tags", json::array())) {
    if (tag.is_string() && tag.get<std::string>().rfind("license:", 0) == 0) {
      return tag.get<std::string>().substr(8);
    }
  }
  return {};
}

bool commercial_license_allowed(std::string license) {
  std::transform(license.begin(), license.end(), license.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  static const std::set<std::string> allowed = {
      "apache-2.0", "mit", "bsd", "bsd-2-clause", "bsd-3-clause", "isc"};
  return allowed.contains(license);
}

json fetch_metadata(const std::string& repo, const std::string& revision = {}) {
  auto url = "https://huggingface.co/api/models/" + repo;
  if (!revision.empty()) url += "/revision/" + revision;
  const auto result = run_command(
      {"curl", "--location", "--fail", "--silent", "--show-error",
       url},
      true);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot read Hugging Face model metadata: " + result.output);
  }
  return json::parse(result.output);
}

std::string capability_modality(const std::string& capability) {
  if (capability == "text") return "text-to-text";
  if (capability == "vision") return "img-text-to-text";
  if (capability == "tts" || capability == "asr") return capability;
  throw std::runtime_error("unsupported model capability for quantization: " + capability);
}

std::string default_mlx_converter(const std::string& modality) {
  if (modality == "text-to-text") return "mlx_lm.convert";
  if (modality == "img-text-to-text") return "mlx_vlm.convert";
  return "mlx_audio.convert";
}

void write_json_atomically(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write " + path.string());
  output << std::setw(2) << value << '\n';
  output.close();
  if (!output) throw std::runtime_error("failed writing " + path.string());
  std::filesystem::rename(temporary, path);
}

json read_runtime(const std::filesystem::path& root) {
  std::ifstream file(root / "mica-server/runtime.json");
  if (!file) throw std::runtime_error("run mica-server setup before quantizing models");
  return json::parse(file);
}

void record_runtime_variant(const std::filesystem::path& root, const std::string& id,
                            Backend backend, Quantization quantization,
                            const std::filesystem::path& artifact,
                            const std::string& source_repo,
                            const std::string& source_revision) {
  const auto path = root / "mica-server/runtime.json";
  std::ifstream input(path);
  auto runtime = json::parse(input);
  auto& model = runtime["configured_models"][id];
  model["enabled"] = true;
  auto& selected = model["variants"][to_string(backend)];
  if (!selected.is_array()) selected = json::array();
  const auto quant = to_string(quantization);
  bool found = false;
  for (const auto& value : selected) {
    if (value.is_string() && value.get<std::string>() == quant) found = true;
  }
  if (!found) selected.push_back(quant);
  const auto key = id + "@" + to_string(backend) + ":" + quant;
  runtime["downloads"][key] = {
      {"model", id}, {"backend", to_string(backend)}, {"quantization", quant},
      {"artifact", artifact.string()}, {"source", "local-quantization"},
      {"source_repo", source_repo}, {"source_revision", source_revision},
      {"downloaded", true}, {"smoke_validated", false}};
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  output << std::setw(2) << runtime << '\n';
  output.close();
  std::filesystem::rename(temporary, path);
}

void record_runtime_selection(const std::filesystem::path& root, const std::string& id,
                              Backend backend, Quantization quantization) {
  const auto path = root / "mica-server/runtime.json";
  std::ifstream input(path);
  auto runtime = json::parse(input);
  auto& model = runtime["configured_models"][id];
  model["enabled"] = true;
  auto& selected = model["variants"][to_string(backend)];
  if (!selected.is_array()) selected = json::array();
  const auto quant = to_string(quantization);
  const bool found = std::any_of(selected.begin(), selected.end(), [&](const auto& value) {
    return value.is_string() && value.template get<std::string>() == quant;
  });
  if (!found) selected.push_back(quant);
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  output << std::setw(2) << runtime << '\n';
  output.close();
  std::filesystem::rename(temporary, path);
}

bool backend_installed(const json& runtime, Backend backend) {
  for (const auto& item : runtime.value("installed_backends", json::array())) {
    if (item.is_string() && item.get<std::string>() == to_string(backend)) return true;
  }
  return false;
}

std::uintmax_t directory_size(const std::filesystem::path& path) {
  if (std::filesystem::is_regular_file(path)) return std::filesystem::file_size(path);
  std::uintmax_t bytes = 0;
  if (!std::filesystem::exists(path)) return bytes;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_regular_file()) bytes += entry.file_size();
  }
  return bytes;
}

double estimated_reservation(const std::filesystem::path& path,
                             const std::filesystem::path& projector = {}) {
  auto bytes = directory_size(path);
  if (!projector.empty()) bytes += directory_size(projector);
  const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  return std::max(0.5, std::ceil((gib * 1.25 + 0.25) * 10.0) / 10.0);
}

std::filesystem::path find_audio_tensor_source(const std::filesystem::path& root) {
  std::filesystem::path first_safetensors;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto filename = entry.path().filename().string();
    if (filename.ends_with(".safetensors.index.json")) return entry.path();
    if (filename == "model.safetensors") first_safetensors = entry.path();
    else if (first_safetensors.empty() && filename.ends_with(".safetensors")) {
      first_safetensors = entry.path();
    }
  }
  if (first_safetensors.empty()) {
    throw std::runtime_error("audio.cpp conversion requires safetensors weights");
  }
  return first_safetensors;
}

std::filesystem::path find_mmproj(const std::filesystem::path& root) {
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().filename().string().rfind("mmproj-", 0) == 0 &&
        entry.path().extension() == ".gguf") return entry.path();
  }
  return {};
}

}  // namespace

std::string normalize_modality(const std::string& value) {
  if (value == "tts" || value == "asr" || value == "text-to-text" ||
      value == "img-text-to-text") return value;
  throw std::invalid_argument(
      "modality must be tts, asr, text-to-text, or img-text-to-text");
}

std::string modality_capability(const std::string& modality) {
  if (modality == "text-to-text") return "text";
  if (modality == "img-text-to-text") return "vision";
  return modality;
}

std::string add_custom_model(const AddModelOptions& options) {
  if (options.root.empty()) throw std::invalid_argument("model root is required");
  const auto modality = normalize_modality(options.modality);
  const auto repo = hugging_face_repo(options.url);
  const auto metadata = fetch_metadata(repo);
  const auto license = model_license(metadata);
  if (!commercial_license_allowed(license)) {
    throw std::runtime_error("model license is missing or not on the commercial-use allowlist: " +
                             (license.empty() ? std::string("unknown") : license));
  }
  const auto id = options.id.empty() ? slug_from_repo(repo) : options.id;
  if (id.empty() || !std::all_of(id.begin(), id.end(), [](char value) {
        return safe_repo_character(value) && value != '.';
      })) {
    throw std::invalid_argument("model id may contain letters, numbers, hyphens, and underscores");
  }
  auto catalog = read_catalog(options.root);
  if (catalog["models"].contains(id)) throw std::runtime_error("model already exists: " + id);
  json entry = {
      {"id", id}, {"source_repo", repo}, {"source_url", "https://huggingface.co/" + repo},
      {"modality", modality}, {"capability", modality_capability(modality)},
      {"license", license}, {"description", options.description}, {"enabled", true},
      {"variants", json::object()}};
  if (options.dry_run) {
    std::cout << std::setw(2) << entry << '\n';
    return id;
  }
  catalog["models"][id] = std::move(entry);
  write_catalog(options.root, catalog);
  std::cout << "Added " << id << " to " << catalog_path(options.root) << '\n';
  return id;
}

void quantize_model(const QuantizeModelOptions& options) {
  if (options.group_size <= 0) throw std::invalid_argument("group size must be positive");
  if (options.quantization == Quantization::native) {
    throw std::invalid_argument("native is only valid for a vLLM source repository");
  }
  auto catalog = read_catalog(options.root);
  const bool custom = catalog["models"].contains(options.id);
  json* custom_model = custom ? &catalog["models"][options.id] : nullptr;
  std::string source_repo;
  std::string modality;
  std::string mlx_converter;
  std::string mlx_quantization_profile;
  std::string configured_gguf_artifact;
  std::string configured_gguf_projector;
  std::string gguf_family;
  bool mlx_extract_mtp = false;
  if (custom) {
    source_repo = custom_model->at("source_repo").get<std::string>();
    modality = custom_model->at("modality").get<std::string>();
    mlx_converter = default_mlx_converter(modality);
  } else {
    if (options.config_directory.empty()) {
      throw std::invalid_argument("config directory is required for a built-in model");
    }
    const auto registry = load_registry(options.config_directory);
    const auto& definition = registry.model(options.id);
    source_repo = definition.source_repo;
    modality = capability_modality(definition.capability);
    mlx_converter = definition.mlx_converter.empty()
                        ? default_mlx_converter(modality)
                        : definition.mlx_converter;
    mlx_quantization_profile = definition.mlx_quantization_profile;
    gguf_family = definition.gguf_family;
    mlx_extract_mtp = definition.mlx_extract_mtp;
    const auto backend = definition.artifacts.find(Backend::gguf);
    if (backend != definition.artifacts.end()) {
      const auto quantization = backend->second.find(options.quantization);
      if (quantization != backend->second.end()) {
        configured_gguf_artifact = quantization->second.pattern;
        configured_gguf_projector = quantization->second.projector_pattern;
      }
    }
  }
  if (source_repo.empty()) {
    throw std::runtime_error("model has no original Hugging Face source repository: " +
                             options.id);
  }
  const auto runtime = read_runtime(options.root);
  if (!backend_installed(runtime, options.backend)) {
    throw std::runtime_error("requested quantization backend is not installed: " +
                             to_string(options.backend));
  }
  const auto backend_name = to_string(options.backend);
  const auto quant_name = to_string(options.quantization);
  json vllm_profiles;
  std::string vllm_algorithm;
  std::string vllm_scheme;
  std::string vllm_candidate;
  int calibration_samples = options.calibration_samples;
  int calibration_sequence_length = options.calibration_sequence_length;
  int calibration_batch_size = options.calibration_batch_size;
  const bool calibration_batch_overridden = calibration_batch_size > 0;
  int auto_round_iterations = options.auto_round_iterations;
  std::string vllm_calibration_mode;
  std::string vllm_toolchain = "current";
  if (options.backend == Backend::vllm) {
    static const std::set<std::string> quantization_devices = {
        "auto", "cpu", "cuda", "xpu"};
    if (!quantization_devices.contains(options.quantization_device)) {
      throw std::invalid_argument(
          "vLLM quantization device must be auto, cpu, cuda, or xpu");
    }
    const auto profiles_path = options.config_directory / "vllm_quantization_profiles.json";
    std::ifstream profiles_file(profiles_path);
    if (!profiles_file) {
      throw std::runtime_error("missing vLLM quantization profiles: " +
                               profiles_path.string());
    }
    vllm_profiles = json::parse(profiles_file);
    if (vllm_profiles.value("schema", 0) != 1) {
      throw std::runtime_error("unsupported vLLM quantization profile schema");
    }
    if (!vllm_profiles.at("tools").contains("python")) {
      throw std::runtime_error("missing pinned vLLM tool: python");
    }
    if (!vllm_profiles.contains("toolchains")) {
      throw std::runtime_error("missing vLLM quantization toolchains");
    }
    const auto& defaults = vllm_profiles.at("defaults").at(quant_name);
    vllm_algorithm = options.vllm_algorithm.empty()
                         ? defaults.at("primary_algorithm").get<std::string>()
                         : options.vllm_algorithm;
    vllm_scheme = options.vllm_scheme.empty()
                       ? defaults.at("scheme").get<std::string>()
                       : options.vllm_scheme;
    if (vllm_algorithm != "auto_round" && vllm_algorithm != "gptq") {
      throw std::invalid_argument("vLLM algorithm must be auto_round or gptq");
    }
    const auto required_scheme = options.quantization == Quantization::q4
                                     ? std::string("W4A16")
                                     : std::string("W8A16");
    if (vllm_scheme != required_scheme) {
      throw std::invalid_argument(quant_name + " requires vLLM scheme " + required_scheme);
    }
    if (calibration_samples <= 0) {
      calibration_samples = defaults.at("calibration_samples").get<int>();
    }
    if (calibration_sequence_length <= 0) {
      calibration_sequence_length =
          defaults.at("calibration_sequence_length").get<int>();
    }
    if (calibration_batch_size <= 0) {
      calibration_batch_size = defaults.at("batch_size").get<int>();
    }
    if (auto_round_iterations <= 0) {
      auto_round_iterations = defaults.at("auto_round_iterations").get<int>();
    }
    const auto model_profiles = vllm_profiles.at("models");
    if (model_profiles.contains(options.id)) {
      const auto& model_profile = model_profiles.at(options.id);
      vllm_toolchain = model_profile.value("toolchain", "current");
      if (options.vllm_algorithm.empty() &&
          model_profile.contains("preferred_algorithms") &&
          model_profile.at("preferred_algorithms").contains(quant_name)) {
        vllm_algorithm =
            model_profile.at("preferred_algorithms").at(quant_name).get<std::string>();
      }
      if (model_profile.at("capability").get<std::string>() !=
          modality_capability(modality)) {
        throw std::runtime_error("vLLM quantization profile capability mismatch for " +
                                 options.id);
      }
      vllm_calibration_mode = model_profile.at("calibration").get<std::string>();
      if (!calibration_batch_overridden &&
          model_profile.contains("calibration_overrides") &&
          model_profile.at("calibration_overrides").contains(quant_name)) {
        calibration_batch_size =
            model_profile.at("calibration_overrides").at(quant_name)
                .value("batch_size", calibration_batch_size);
      }
      if (vllm_calibration_mode == "custom-required") {
        throw std::runtime_error(
            options.id +
            " requires a modality-specific calibration adapter; source download and "
            "text-only vLLM quantization are refused");
      }
    } else {
      vllm_calibration_mode =
          modality_capability(modality) == "text" ? "chat-text" : "custom-required";
      if (vllm_calibration_mode == "custom-required") {
        throw std::runtime_error(
            options.id +
            " requires a modality-specific calibration adapter; source download and "
            "text-only vLLM quantization are refused");
      }
    }
    if (vllm_calibration_mode != "chat-text" &&
        options.calibration_dataset.empty()) {
      throw std::runtime_error(
          options.id +
          " requires --calibration-dataset with an audited modality JSONL manifest");
    }
    if (vllm_calibration_mode != "chat-text" &&
        vllm_algorithm == "auto_round") {
      throw std::runtime_error(
          "AutoRound CLI cannot consume Mica modality manifests; use GPTQ");
    }
    if (vllm_algorithm != "auto_round" && vllm_algorithm != "gptq") {
      throw std::invalid_argument("vLLM algorithm must be auto_round or gptq");
    }
    if (!vllm_profiles.at("toolchains").contains(vllm_toolchain)) {
      throw std::runtime_error("missing vLLM quantization toolchain: " +
                               vllm_toolchain);
    }
    const auto& selected_tools = vllm_profiles.at("toolchains").at(vllm_toolchain);
    for (const auto& field : {"transformers", "datasets", "auto_round",
                              "compressed_tensors", "llmcompressor"}) {
      if (!selected_tools.contains(field)) {
        throw std::runtime_error(std::string("missing pinned vLLM toolchain field: ") +
                                 field);
      }
    }
    auto scheme_slug = vllm_scheme;
    std::transform(scheme_slug.begin(), scheme_slug.end(), scheme_slug.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    vllm_candidate = vllm_algorithm + "-" + scheme_slug + "-g" +
                     std::to_string(vllm_profiles.at("group_size").get<int>());
  }
  if (custom && custom_model->contains("variants") &&
      (*custom_model)["variants"].contains(backend_name) &&
      (*custom_model)["variants"][backend_name].contains(quant_name) &&
      (*custom_model)["variants"][backend_name][quant_name].value("status", "") ==
          "ready") {
    throw std::runtime_error("quantized variant already exists: " + options.id + "@" +
                             backend_name + ":" + quant_name);
  }
  const int bits = options.quantization == Quantization::q4 ? 4 : 8;
  const auto output_root = options.root / "checkpoints" / to_string(options.backend) /
                           options.id;
  const auto marker = output_root /
      (options.backend == Backend::vllm
           ? ".mica-candidate-" + vllm_candidate
           : ".mica-complete-" + quant_name);
  if (std::filesystem::exists(marker)) {
    throw std::runtime_error("quantized variant already exists: " + options.id + "@" +
                             backend_name + ":" + quant_name);
  }

  json metadata;
  auto source_revision = options.source_revision;
  std::string license;
  if (!options.dry_run) {
    metadata = fetch_metadata(source_repo, source_revision);
    license = model_license(metadata);
    if (!commercial_license_allowed(license)) {
      throw std::runtime_error(
          "source license is missing or not on the commercial-use allowlist: " +
          (license.empty() ? std::string("unknown") : license));
    }
    source_revision = metadata.value("sha", source_revision);
    if (source_revision.empty()) {
      throw std::runtime_error("Hugging Face did not return an immutable source revision");
    }
  } else if (source_revision.empty()) {
    source_revision = "resolved-revision";
  }
  const auto staging = options.root / "staging" / options.id / source_revision;
  const auto source = staging / "source";
  const auto hf = (options.root / "environment-tools/bin/hf").string();
  run_or_print({hf, "download", source_repo, "--revision", source_revision,
                "--local-dir", source.string()},
               options.dry_run);
  if (!options.dry_run) {
    write_json_atomically(
        staging / "source-provenance.json",
        {{"schema", 1},
         {"model", options.id},
         {"source_repo", source_repo},
         {"source_revision", source_revision},
         {"source_url", "https://huggingface.co/" + source_repo + "/tree/" +
                            source_revision},
         {"license", license},
         {"snapshot_bytes", directory_size(source)}});
  }
  std::filesystem::path artifact;
  std::filesystem::path projector;
  std::filesystem::path drafter;

  if (options.backend == Backend::vllm) {
    artifact = output_root / "candidates" / vllm_candidate;
    const auto script = options.config_directory.parent_path() /
                        "scripts/vllm_quantize.py";
    const auto profiles_path = options.config_directory /
                               "vllm_quantization_profiles.json";
    if (!options.dry_run && !std::filesystem::exists(script)) {
      throw std::runtime_error("missing vLLM quantizer: " + script.string());
    }
    const auto& global_tools = vllm_profiles.at("tools");
    const auto& tools = vllm_profiles.at("toolchains").at(vllm_toolchain);
    const auto resource_policy = vllm_profiles.at("resource_policy");
    const auto memory_limit = resource_policy.at("max_process_tree_rss_gib").get<double>();
    const auto max_threads = resource_policy.at("max_threads").get<int>();
    const auto memory_wrapper = options.config_directory.parent_path() /
                                "scripts/run_memory_limited.py";
    if (!options.dry_run && !std::filesystem::exists(memory_wrapper)) {
      throw std::runtime_error("missing conversion memory watchdog: " +
                               memory_wrapper.string());
    }
    std::vector<std::string> command = {
        "python3", memory_wrapper.string(), "--limit-gib", std::to_string(memory_limit),
        "--", "env", "OMP_NUM_THREADS=" + std::to_string(max_threads),
        "VECLIB_MAXIMUM_THREADS=" + std::to_string(max_threads),
        "TOKENIZERS_PARALLELISM=false",
        "UV_CACHE_DIR=" + (options.root / "cache/uv").string(),
        "HF_HOME=" + (options.root / "cache/huggingface").string(),
        "uv", "run", "--isolated", "--python",
        global_tools.at("python").get<std::string>()};
    if (vllm_algorithm == "auto_round") {
      command.insert(command.end(), {
          "--with", "auto-round==" + tools.at("auto_round").get<std::string>(),
          "--with", "compressed-tensors==" +
                        tools.at("compressed_tensors").get<std::string>()});
    } else {
      command.insert(command.end(), {
          "--with", "llmcompressor==" + tools.at("llmcompressor").get<std::string>()});
    }
    command.insert(command.end(), {
        "--with", "datasets==" + tools.at("datasets").get<std::string>(),
        "--with", "transformers==" + tools.at("transformers").get<std::string>()});
    const auto& model_profile = vllm_profiles.at("models").contains(options.id)
                                    ? vllm_profiles.at("models").at(options.id)
                                    : json::object();
    if (model_profile.contains("calibration_dependencies")) {
      for (const auto& [package, version] :
           model_profile.at("calibration_dependencies").items()) {
        command.insert(command.end(), {
            "--with", package + "==" + version.get<std::string>()});
      }
    }
    command.insert(command.end(), {
        "--", "python", script.string(),
        "--profiles", profiles_path.string(),
        "--model-id", options.id,
        "--capability", modality_capability(modality),
        "--source", source.string(),
        "--source-repo", source_repo,
        "--source-revision", source_revision,
        "--source-license", license.empty() ? "unknown" : license,
        "--output", artifact.string(),
        "--quant", quant_name,
        "--algorithm", vllm_algorithm,
        "--scheme", vllm_scheme,
        "--device", options.quantization_device,
        "--calibration-samples", std::to_string(calibration_samples),
        "--calibration-sequence-length", std::to_string(calibration_sequence_length),
        "--batch-size", std::to_string(calibration_batch_size),
        "--iterations", std::to_string(auto_round_iterations)});
    if (!options.calibration_dataset.empty()) {
      command.insert(command.end(), {"--dataset", options.calibration_dataset});
    }
    if (!options.calibration_dataset_split.empty()) {
      command.insert(command.end(), {"--dataset-split", options.calibration_dataset_split});
    }
    run_or_print(command, options.dry_run);
    if (options.dry_run) return;
    if (!std::filesystem::exists(artifact / "mica-vllm-candidate.json")) {
      throw std::runtime_error("vLLM quantizer completed without candidate metadata: " +
                               artifact.string());
    }
    std::filesystem::create_directories(output_root);
    std::ofstream marker_file(marker, std::ios::trunc);
    marker_file << source_repo << '@' << source_revision << '\n';
    std::cout << "Created unvalidated vLLM candidate " << options.id << '@'
              << vllm_candidate << " at " << artifact << '\n';
    return;
  }

  if (options.backend == Backend::mlx) {
    artifact = output_root / to_string(options.quantization);
    const auto python = (options.root / "environment-mlx/bin/python").string();
    std::vector<std::string> command;
    if (!mlx_quantization_profile.empty()) {
      const auto script = options.config_directory.parent_path() /
                          "scripts/mlx_audio_convert.py";
      const auto profile_config = options.config_directory /
                                  "mlx_audio_profiles.json";
      if (!options.dry_run && !std::filesystem::exists(script)) {
        throw std::runtime_error("missing MLX-audio profile converter: " +
                                 script.string());
      }
      if (!options.dry_run && !std::filesystem::exists(profile_config)) {
        throw std::runtime_error("missing MLX-audio profile definitions: " +
                                 profile_config.string());
      }
      command = {python, script.string(), "--profile", mlx_quantization_profile,
                 "--profile-config", profile_config.string(),
                 "--hf-path", source.string(), "--mlx-path", artifact.string(),
                 "--q-bits", std::to_string(bits), "--q-group-size",
                 std::to_string(options.group_size), "--model-id", options.id,
                 "--source-repo", source_repo, "--source-revision", source_revision,
                 "--source-license", license.empty() ? "unknown" : license};
    } else {
      command = {python, "-m", mlx_converter, "--hf-path", source.string(),
                 "--mlx-path", artifact.string(), "--quantize", "--q-bits",
                 std::to_string(bits), "--q-group-size",
                 std::to_string(options.group_size)};
    }
    if (mlx_extract_mtp) {
      drafter = output_root / ("mtp-" + quant_name);
      command.insert(command.end(), {"--mtp", "--mtp-output", drafter.string()});
    }
    run_or_print(command, options.dry_run);
  } else {
    const auto python = (options.root / "environment-tools/bin/python").string();
    if (modality == "tts" || modality == "asr") {
      artifact = output_root /
                 (configured_gguf_artifact.empty()
                      ? options.id + "-" +
                            (options.quantization == Quantization::q4 ? "q4_k" : "q8_0") +
                            ".gguf"
                      : configured_gguf_artifact);
      if (!options.dry_run) std::filesystem::create_directories(output_root);
      const auto audio_cpp = options.root / "runtime/audio.cpp";
      const auto converter = audio_cpp / "build-mica/bin/audiocpp_gguf";
      const auto quant_type =
          options.quantization == Quantization::q4 ? "q4_k" : "q8_0";
      std::vector<std::string> command;
      if (gguf_family == "audio8_tts") {
        const auto codec_safetensors = source / "codec.safetensors";
        if (options.dry_run || !std::filesystem::exists(codec_safetensors)) {
          run_or_print(
              {python,
               (audio_cpp / "tools/community_models/convert_audio8_tts_codec.py").string(),
               (source / "codec.pth").string(), codec_safetensors.string()},
              options.dry_run);
        }
        command = {converter.string(),
                   "--input", "model_weights=" + (source / "model.safetensors").string(),
                   "--input", "codec_weights=" + codec_safetensors.string(),
                   "--root", source.string(),
                   "--model-spec", (audio_cpp / "model_specs/audio8_tts.json").string(),
                   "--output", artifact.string(), "--type",
                   options.quantization == Quantization::q4 ? "q4_0" : "q8_0",
                   "--family", "audio8_tts"};
      } else {
        const auto tensor_source = options.dry_run
                                       ? source / "model.safetensors"
                                       : find_audio_tensor_source(source);
        command = {converter.string(), "--input", tensor_source.string(),
                   "--root", source.string(), "--output", artifact.string(),
                   "--type", quant_type};
      }
      if (!gguf_family.empty() && gguf_family != "audio8_tts") {
        command.insert(command.end(), {"--family", gguf_family});
      }
      run_or_print(command, options.dry_run);
    } else {
      const auto llama = options.root / "runtime/llama.cpp";
      if (!options.dry_run) std::filesystem::create_directories(output_root);
      run_or_print({"uv", "pip", "install", "--python", python, "-r",
                    (llama / "requirements.txt").string()}, options.dry_run);
      const auto base = staging / (options.id + "-f16.gguf");
      run_or_print({python, (llama / "convert_hf_to_gguf.py").string(), source.string(),
                    "--outfile", base.string(), "--outtype", "f16"}, options.dry_run);
      if (modality == "img-text-to-text") {
        run_or_print({python, (llama / "convert_hf_to_gguf.py").string(), source.string(),
                      "--outfile", base.string(), "--outtype", "f16", "--mmproj"},
                     options.dry_run);
        if (!options.dry_run) {
          const auto generated_projector = find_mmproj(staging);
          if (!generated_projector.empty()) {
            projector = output_root /
                        (configured_gguf_projector.empty()
                             ? generated_projector.filename()
                             : std::filesystem::path(configured_gguf_projector));
            std::filesystem::copy_file(generated_projector, projector);
          }
        }
      }
      artifact = output_root /
                 (configured_gguf_artifact.empty()
                      ? options.id + "-" +
                            (options.quantization == Quantization::q4 ? "Q4_K_M" : "Q8_0") +
                            ".gguf"
                      : configured_gguf_artifact);
      run_or_print({(llama / "build-mica/bin/llama-quantize").string(), base.string(),
                    artifact.string(),
                    options.quantization == Quantization::q4 ? "Q4_K_M" : "Q8_0"},
                   options.dry_run);
      if (modality == "img-text-to-text" && !options.dry_run && projector.empty()) {
        throw std::runtime_error("llama.cpp did not produce an mmproj GGUF for this vision model");
      }
    }
  }

  if (options.dry_run) return;
  if (!std::filesystem::exists(artifact)) {
    throw std::runtime_error("quantizer completed without output: " + artifact.string());
  }
  if (mlx_extract_mtp && !std::filesystem::exists(drafter)) {
    throw std::runtime_error("source declares MTP but converter produced no drafter: " +
                             drafter.string());
  }
  std::ofstream marker_file(marker, std::ios::trunc);
  marker_file << source_repo << '@' << source_revision << '\n';
  marker_file.close();
  const auto relative = std::filesystem::relative(artifact, output_root).string();
  json variant = {{"status", "ready"}, {"artifact", relative},
                  {"reservation_gib", estimated_reservation(artifact, projector)},
                  {"reservation_source", "provisional-file-size-estimate"},
                  {"group_size", options.group_size}};
  if (!projector.empty()) {
    variant["projector"] = std::filesystem::relative(projector, output_root).string();
  }
  if (!drafter.empty()) {
    variant["drafter"] = std::filesystem::relative(drafter, output_root).string();
  }
  if (custom) {
    (*custom_model)["variants"][backend_name][quant_name] = variant;
    write_catalog(options.root, catalog);
  }
  record_runtime_variant(options.root, options.id, options.backend, options.quantization,
                         artifact, source_repo, source_revision);
  std::cout << "Quantized " << options.id << " for " << to_string(options.backend) << "/"
            << to_string(options.quantization) << " at " << artifact << '\n';
}

void configure_vllm_custom_model(const ConfigureVllmModelOptions& options) {
  if (options.reservation_gib <= 0) {
    throw std::invalid_argument("vLLM model memory reservation must be positive");
  }
  auto catalog = read_catalog(options.root);
  if (!catalog["models"].contains(options.id)) {
    throw std::runtime_error("custom model is not registered: " + options.id);
  }
  const auto runtime = read_runtime(options.root);
  if (!backend_installed(runtime, Backend::vllm)) {
    throw std::runtime_error("vLLM backend is not installed");
  }
  auto& model = catalog["models"][options.id];
  const auto modality = model.at("modality").get<std::string>();
  if (modality == "tts") {
    throw std::runtime_error(
        "TTS requires a vLLM-Omni worker, which is not installed by this backend yet");
  }
  auto& variant = model["variants"]["vllm"]["native"];
  if (variant.is_object() && variant.value("status", "") == "ready") {
    throw std::runtime_error("vLLM native variant already exists: " + options.id);
  }
  variant = {{"status", "ready"},
             {"artifact", "model"},
             {"reservation_gib", options.reservation_gib},
             {"reservation_source", "user-declared"}};
  if (options.dry_run) {
    std::cout << std::setw(2) << variant << '\n';
    return;
  }
  write_catalog(options.root, catalog);
  record_runtime_selection(options.root, options.id, Backend::vllm,
                           Quantization::native);
  std::cout << "Configured " << options.id
            << " as a lazy vLLM Hugging Face source with a "
            << options.reservation_gib << " GiB reservation\n";
}

void merge_custom_models(Registry& registry, const std::filesystem::path& root) {
  const auto catalog = read_catalog(root);
  for (const auto& [id, entry] : catalog["models"].items()) {
    if (!entry.value("enabled", true)) continue;
    const auto duplicate = std::find_if(registry.models.begin(), registry.models.end(),
                                        [&](const auto& model) { return model.id == id; });
    if (duplicate != registry.models.end()) {
      throw std::runtime_error("custom model id conflicts with built-in model: " + id);
    }
    ModelDefinition model;
    model.id = id;
    model.capability = entry.at("capability").get<std::string>();
    model.description = entry.value("description", std::string());
    model.tags = {entry.at("modality").get<std::string>(),
                  "license:" + entry.at("license").get<std::string>(), "custom"};
    model.source_repo = entry.at("source_repo").get<std::string>();
    model.repositories[Backend::mlx] = model.source_repo;
    model.repositories[Backend::gguf] = model.source_repo;
    model.repositories[Backend::vllm] = model.source_repo;
    model.startup_priority = 100;
    for (const auto backend : {Backend::mlx, Backend::gguf, Backend::vllm}) {
      for (const auto quantization :
           {Quantization::q4, Quantization::q8, Quantization::native}) {
        Artifact artifact;
        artifact.reason = "custom model has not been quantized for " + to_string(backend) + "/" +
                          to_string(quantization);
        const auto backend_name = to_string(backend);
        const auto quant_name = to_string(quantization);
        if (entry.contains("variants") && entry["variants"].contains(backend_name) &&
            entry["variants"][backend_name].contains(quant_name)) {
          const auto& variant = entry["variants"][backend_name][quant_name];
          artifact.supported = variant.value("status", "") == "ready";
          artifact.pattern = variant.value("artifact", "");
          artifact.projector_pattern = variant.value("projector", "");
          artifact.reservation_gib = variant.value("reservation_gib", 0.0);
          if (artifact.supported) artifact.reason.clear();
        }
        model.artifacts[backend][quantization] = std::move(artifact);
      }
    }
    registry.models.push_back(std::move(model));
    for (auto& [_, profile] : registry.profiles) profile.models.push_back(id);
  }
}

nlohmann::json registry_catalog(const Registry& registry,
                                const std::optional<std::string>& capability,
                                const std::optional<Backend>& backend,
                                bool check_remote) {
  json models = json::array();
  std::map<std::string, bool> remote_status;
  const auto modality_for = [](const std::string& value) {
    if (value == "text") return std::string("text-to-text");
    if (value == "vision") return std::string("image-video-to-text");
    return value;
  };
  for (const auto& model : registry.models) {
    if (capability && model.capability != *capability) continue;
    json repositories = json::object();
    json revisions = json::object();
    json variants = json::object();
    for (const auto& [candidate_backend, artifacts] : model.artifacts) {
      if (backend && candidate_backend != *backend) continue;
      json quantizations = json::array();
      for (const auto& [quantization, artifact] : artifacts) {
        if (artifact.supported) quantizations.push_back(to_string(quantization));
      }
      if (quantizations.empty()) continue;
      const auto repository = model.repositories.find(candidate_backend);
      if (repository == model.repositories.end() || repository->second.empty()) continue;
      const auto backend_name = to_string(candidate_backend);
      variants[backend_name] = quantizations;
      repositories[backend_name] = repository->second;
      const auto revision = model.repository_revisions.find(candidate_backend);
      revisions[backend_name] = revision == model.repository_revisions.end()
                                    ? "main"
                                    : revision->second;
      if (check_remote && !remote_status.contains(repository->second)) {
        const auto result = run_command(
            {"curl", "--head", "--location", "--fail", "--silent",
             "--show-error", "--max-time", "10",
             "https://huggingface.co/" + repository->second},
            true);
        remote_status[repository->second] = result.exit_code == 0;
      }
    }
    if (variants.empty()) continue;
    json item = {{"id", model.id},
                 {"capability", model.capability},
                 {"modality", modality_for(model.capability)},
                 {"description", model.description},
                 {"tags", model.tags},
                 {"source_repository", model.source_repo},
                 {"repositories", repositories},
                 {"revisions", revisions},
                 {"variants", variants}};
    if (check_remote) {
      json availability = json::object();
      for (const auto& [backend_name, repository] : repositories.items()) {
        availability[backend_name] = {
            {"repository", repository},
            {"available", remote_status.at(repository.get<std::string>())}};
      }
      item["remote"] = std::move(availability);
    }
    models.push_back(std::move(item));
  }
  return {{"schema", 1}, {"object", "model_registry"}, {"data", models}};
}

}  // namespace mica

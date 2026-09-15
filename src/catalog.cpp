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

json fetch_metadata(const std::string& repo) {
  const auto result = run_command(
      {"curl", "--location", "--fail", "--silent", "--show-error",
       "https://huggingface.co/api/models/" + repo},
      true);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot read Hugging Face model metadata: " + result.output);
  }
  return json::parse(result.output);
}

json read_runtime(const std::filesystem::path& root) {
  std::ifstream file(root / "mica-server/runtime.json");
  if (!file) throw std::runtime_error("run mica-server setup before quantizing models");
  return json::parse(file);
}

void record_runtime_variant(const std::filesystem::path& root, const std::string& id,
                            Backend backend, Quantization quantization,
                            const std::filesystem::path& artifact) {
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

void quantize_custom_model(const QuantizeModelOptions& options) {
  if (options.group_size <= 0) throw std::invalid_argument("group size must be positive");
  if (options.backend == Backend::vllm) {
    throw std::invalid_argument(
        "vLLM is a serving backend, not a generic Q4/Q8 converter; quantize with MLX or "
        "GGUF, or configure a vLLM-supported AWQ/GPTQ/compressed-tensors repository");
  }
  if (options.quantization == Quantization::native) {
    throw std::invalid_argument("native is only valid for a vLLM source repository");
  }
  auto catalog = read_catalog(options.root);
  if (!catalog["models"].contains(options.id)) {
    throw std::runtime_error("custom model is not registered: " + options.id);
  }
  const auto runtime = read_runtime(options.root);
  if (!backend_installed(runtime, options.backend)) {
    throw std::runtime_error("requested quantization backend is not installed: " +
                             to_string(options.backend));
  }
  auto& model = catalog["models"][options.id];
  const auto backend_name = to_string(options.backend);
  const auto quant_name = to_string(options.quantization);
  if (model.contains("variants") && model["variants"].contains(backend_name) &&
      model["variants"][backend_name].contains(quant_name) &&
      model["variants"][backend_name][quant_name].value("status", "") == "ready") {
    throw std::runtime_error("quantized variant already exists: " + options.id + "@" +
                             backend_name + ":" + quant_name);
  }
  const auto source_repo = model.at("source_repo").get<std::string>();
  const auto modality = model.at("modality").get<std::string>();
  const int bits = options.quantization == Quantization::q4 ? 4 : 8;
  const auto output_root = options.root / "checkpoints" / to_string(options.backend) /
                           options.id;
  const auto staging = options.root / "staging" / options.id;
  std::filesystem::path artifact;
  std::filesystem::path projector;

  if (options.backend == Backend::mlx) {
    artifact = output_root / to_string(options.quantization);
    const auto python = (options.root / "environment-mlx/bin/python").string();
    std::string module;
    if (modality == "text-to-text") module = "mlx_lm.convert";
    else if (modality == "img-text-to-text") module = "mlx_vlm.convert";
    else module = "mlx_audio.convert";
    run_or_print({python, "-m", module, "--hf-path", source_repo, "--mlx-path",
                  artifact.string(), "--quantize", "--q-bits", std::to_string(bits),
                  "--q-group-size", std::to_string(options.group_size)},
                 options.dry_run);
  } else {
    const auto hf = (options.root / "environment-tools/bin/hf").string();
    const auto python = (options.root / "environment-tools/bin/python").string();
    const auto source = staging / "source";
    run_or_print({hf, "download", source_repo, "--local-dir", source.string()},
                 options.dry_run);
    if (modality == "tts" || modality == "asr") {
      artifact = output_root /
                 (options.id + "-" +
                  (options.quantization == Quantization::q4 ? "q4_k" : "q8_0") + ".gguf");
      if (!options.dry_run) std::filesystem::create_directories(output_root);
      const auto tensor_source = options.dry_run
                                     ? source / "model.safetensors"
                                     : find_audio_tensor_source(source);
      run_or_print({(options.root / "runtime/audio.cpp/build-mica/bin/audiocpp_gguf").string(),
                    "--input", tensor_source.string(), "--root", source.string(), "--output",
                    artifact.string(), "--type",
                    options.quantization == Quantization::q4 ? "q4_k" : "q8_0"},
                   options.dry_run);
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
            projector = output_root / generated_projector.filename();
            std::filesystem::copy_file(generated_projector, projector);
          }
        }
      }
      artifact = output_root /
                 (options.id + "-" +
                  (options.quantization == Quantization::q4 ? "Q4_K_M" : "Q8_0") + ".gguf");
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
  const auto marker = output_root / (".mica-complete-" + to_string(options.quantization));
  std::ofstream marker_file(marker, std::ios::trunc);
  marker_file << source_repo << '\n';
  marker_file.close();
  const auto relative = std::filesystem::relative(artifact, output_root).string();
  json variant = {{"status", "ready"}, {"artifact", relative},
                  {"reservation_gib", estimated_reservation(artifact, projector)},
                  {"reservation_source", "provisional-file-size-estimate"},
                  {"group_size", options.group_size}};
  if (!projector.empty()) {
    variant["projector"] = std::filesystem::relative(projector, output_root).string();
  }
  model["variants"][to_string(options.backend)][to_string(options.quantization)] = variant;
  write_catalog(options.root, catalog);
  record_runtime_variant(options.root, options.id, options.backend, options.quantization,
                         artifact);
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

}  // namespace mica

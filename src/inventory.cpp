#include "mica_server/inventory.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/scheduler.hpp"
#include <algorithm>
#include <fstream>

namespace mica {
using json = nlohmann::json;
std::filesystem::path model_cache_directory(const std::filesystem::path& root,
    const ModelDefinition& model, Backend backend, const std::string& engine) {
  auto directory = root / "models" / to_string(backend) / model.id;
  int engines = 0;
  const auto& artifacts = model.artifacts.at(backend);
  if (!artifacts.empty()) for (const auto& [id, variants] : model.engine_artifacts)
    if (!variants.empty() && variants.begin()->second.format == artifacts.begin()->second.format) ++engines;
  if (engines > 1) directory /= engine;
  return directory;
}

json inspect_engine(const EngineDefinition& engine, const HardwareInfo& hardware,
                    const std::filesystem::path& root) {
  std::vector<std::string> targets{"cpu"};
  if (hardware.apple_silicon) targets.push_back("apple-silicon");
  for (const auto& device : hardware.accelerators) {
    auto runtime = device.runtime;
    if (runtime == "hip") runtime = "rocm";
    targets.push_back(runtime);
    for (const auto& api : device.apis) targets.push_back(api);
  }
  bool compatible = false;
  for (const auto& target : engine.hardware)
    compatible |= std::find(targets.begin(), targets.end(), target) != targets.end();
  if (engine.backend == Backend::mlx) compatible &= hardware.supports_mlx();
  if (engine.backend == Backend::gguf) compatible &= hardware.supports_gguf();
  if (engine.backend == Backend::vllm) compatible &= hardware.supports_vllm();
  std::filesystem::path path;
  bool installed = false;
  if (engine.backend == Backend::gguf) {
    path = root / "runtimes" / engine.runtime_directory / engine.server_executable;
    installed = std::filesystem::is_regular_file(path) &&
        (std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
  } else {
    path = root / "environments" / engine.environment_group / "bin/python";
    const auto lib = root / "environments" / engine.environment_group / "lib";
    std::string module = engine.backend == Backend::vllm ? "vllm" :
        engine.id == "mlx-lm" ? "mlx_lm" : engine.id == "mlx-vlm" ? "mlx_vlm" : "mlx_audio";
    std::vector<std::string> modules{module};
    for (const auto& package : engine.python_packages) {
      auto name = package.substr(0, package.find_first_of("[<>=!~ "));
      std::replace(name.begin(), name.end(), '-', '_');
      if (!name.empty()) modules.push_back(name);
    }
    if (std::filesystem::exists(path) && std::filesystem::is_directory(lib))
      for (const auto& entry : std::filesystem::directory_iterator(lib))
        installed |= std::all_of(modules.begin(), modules.end(), [&](const auto& name) {
          return std::filesystem::is_directory(entry.path() / "site-packages" / name);
        });
  }
  return {{"compatible", compatible}, {"installed", installed}, {"path", path.string()},
          {"reason", compatible ? "Supported hardware target detected; inference is not certified by this check" : "No supported hardware target on this machine"}};
}

json inspect_artifact(const ModelDefinition& model, const Artifact& artifact,
    Backend backend, Quantization quantization, const std::filesystem::path& root) {
  const auto primary = model_cache_directory(root, model, backend, artifact.engine) / artifact.pattern;
  const auto base = primary.parent_path();
  const auto marker = base / (".mica-complete-" + to_string(quantization));
  bool cached = std::filesystem::exists(primary) && std::filesystem::is_regular_file(marker);
  json components = json::array();
  for (const auto& file : artifact.files) {
    const bool present = std::filesystem::exists(base / file.path);
    cached &= present;
    components.push_back({{"role", file.role}, {"path", (base / file.path).string()},
                          {"cached", present}, {"size_bytes", file.size_bytes}});
  }
  if (!artifact.projector_pattern.empty()) cached &= std::filesystem::exists(base / artifact.projector_pattern);
  if (cached) {
    std::ifstream stream(marker);
    std::string repository, revision; std::getline(stream, repository); std::getline(stream, revision);
    const auto expected_repository = !artifact.files.empty() ? artifact.files.front().repository :
        model.repositories.contains(backend) ? model.repositories.at(backend) : "";
    const auto expected_revision = !artifact.files.empty() ? artifact.files.front().revision :
        model.repository_revisions.contains(backend) ? model.repository_revisions.at(backend) : "main";
    cached = repository == expected_repository &&
        (revision == expected_revision || (revision.empty() && expected_revision == "main"));
  }
  return {{"cached", cached}, {"path", primary.string()}, {"components", components},
          {"reason", cached ? "Complete cached bundle; no checksums rehashed by inventory" : "Missing files or completion marker/revision mismatch"}};
}

json inspect_workload(const Registry& registry, const Profile& source,
    const HardwareInfo& hardware, const std::filesystem::path& root) {
  auto profile = source;
  json models = json::array();
  bool compatible = true, installed = true, cached = true;
  std::string reason = "All selected model/engine hardware targets are supported";
  try {
    auto resolved_registry = registry;
    resolve_profile_engines(resolved_registry, profile, hardware, root);
    for (const auto& policy : profile.model_policies) {
      const auto& model = registry.model(policy.id);
      const auto& engine = registry.engine(policy.engine);
      const auto& artifact = model.artifact_for(policy.backend, policy.quantization, policy.engine);
      const auto runtime = inspect_engine(engine, hardware, root);
      const auto bundle = inspect_artifact(model, artifact, policy.backend, policy.quantization, root);
      bool supported = runtime.at("compatible").get<bool>();
      if (policy.placement_mode == "fixed" && policy.device != "auto") {
        if (policy.device == "cpu") supported &= policy.backend != Backend::mlx &&
            std::find(engine.hardware.begin(), engine.hardware.end(), "cpu") != engine.hardware.end();
        else {
          bool found = false;
          const auto colon = policy.device.find(':');
          const auto requested_runtime = policy.device.substr(0, colon);
          const auto requested_id = colon == std::string::npos ? policy.device : policy.device.substr(colon + 1);
          const auto placement = resolve_model_placement(policy, artifact, hardware);
          auto target = placement.device.substr(0, placement.device.find(':'));
          if (target == "hip") target = "rocm";
          if (policy.backend == Backend::mlx && hardware.apple_silicon) target = "apple-silicon";
          const bool engine_supports_device = std::find(engine.hardware.begin(), engine.hardware.end(), target) != engine.hardware.end();
          for (const auto& device : hardware.accelerators) {
            found |= engine_supports_device && device.id == requested_id &&
                (requested_runtime == "accelerator" || requested_runtime == device.runtime);
          }
          supported &= found;
        }
      }
      compatible &= supported;
      installed &= runtime.at("installed").get<bool>(); cached &= bundle.at("cached").get<bool>();
      if (!supported) reason = policy.id + ": selected engine or fixed device is unavailable";
      models.push_back({{"id", policy.id}, {"engine", policy.engine}, {"quantization", to_string(policy.quantization)},
                        {"compatible", supported}, {"engine_installed", runtime["installed"]}, {"cached", bundle["cached"]}});
    }
  } catch (const std::exception& error) { compatible = installed = cached = false; reason = error.what(); }
  return {{"compatible", compatible}, {"engines_installed", installed}, {"models_cached", cached},
          {"prepared", installed && cached}, {"reason", reason}, {"models", models}};
}
}

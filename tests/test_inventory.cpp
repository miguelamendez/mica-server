#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mica_server/config.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/inventory.hpp"
#include <cassert>
#include <fstream>
#include <unistd.h>

int main() {
  namespace fs = std::filesystem;
  const auto repo = fs::path(__FILE__).parent_path().parent_path();
  auto registry = mica::load_registry(repo / "config");
  const auto mac = mica::load_hardware_profile(repo / "tests/fixtures/hardware/mac-metal.json");
  const auto cpu = mica::load_hardware_profile(repo / "tests/fixtures/hardware/linux-cpu.json");
  const auto root = fs::temp_directory_path() / ("mica-inventory-test-" + std::to_string(getpid()));
  assert(!fs::exists(root)); fs::create_directory(root);
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
  const auto& mlx = registry.engine("mlx-vlm");
  assert(mica::inspect_engine(mlx, mac, root).at("compatible") == true);
  assert(mica::inspect_engine(mlx, cpu, root).at("compatible") == false);
  assert(mica::inspect_engine(registry.engine("llama-cpp"), cpu, root).at("compatible") == true);
  auto env = root / "environments/mlx";
  fs::create_directories(env / "bin"); std::ofstream(env / "bin/python") << "fixture";
  assert(mica::inspect_engine(mlx, mac, root).at("installed") == false);
  fs::create_directories(env / "lib/python3.12/site-packages/mlx_vlm");
  assert(mica::inspect_engine(mlx, mac, root).at("installed") == true);
  assert(mica::inspect_engine(registry.engine("mlx-audio"), mac, root).at("installed") == false);
  assert(mica::inspect_workload(registry, registry.profile("mac_coder"), cpu, root).at("compatible") == false);
  assert(mica::inspect_workload(registry, registry.profile("mac_coder"), mac, root).at("compatible") == true);
  assert(mica::inspect_workload(registry, registry.profile("gpu_16g_coder"), mac, root).at("compatible") == false);
  auto& model = registry.model("qwen35-4b");
  const auto& artifact = model.artifact_for(mica::Backend::gguf, mica::Quantization::q4, "llama-cpp");
  const auto primary = mica::model_cache_directory(root, model, mica::Backend::gguf, artifact.engine) / artifact.pattern;
  fs::create_directories(primary.parent_path()); std::ofstream(primary) << "fixture";
  const auto marker = primary.parent_path() / ".mica-complete-q4";
  std::ofstream(marker) << artifact.files.front().repository << '\n' << artifact.files.front().revision << '\n';
  // A target alone is not a complete multimodal artifact.
  assert(mica::inspect_artifact(model, artifact, mica::Backend::gguf, mica::Quantization::q4, root).at("cached") == false);
  for (const auto& file : artifact.files) {
    auto path = primary.parent_path() / file.path; fs::create_directories(path.parent_path()); std::ofstream(path) << "fixture";
  }
  assert(mica::inspect_artifact(model, artifact, mica::Backend::gguf, mica::Quantization::q4, root).at("cached") == true);
  std::ofstream(marker) << "wrong/repository\nwrong-revision\n";
  assert(mica::inspect_artifact(model, artifact, mica::Backend::gguf, mica::Quantization::q4, root).at("cached") == false);
}

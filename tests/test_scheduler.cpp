#include <cassert>
#include <fstream>
#include <iostream>

#include <unistd.h>

#include "mica_server/catalog.hpp"
#include "mica_server/config.hpp"
#include "mica_server/scheduler.hpp"

int main() {
  const auto config = std::filesystem::path(__FILE__).parent_path().parent_path() / "config";
  const auto registry = mica::load_registry(config);
  const auto& all = registry.profile("all");
  const auto& spark = registry.model("spark-x25-4b");
  assert(spark.repositories.at(mica::Backend::mlx) ==
         "miguelamendez/mica-spark-x25-4b-mlx");
  assert(spark.repositories.at(mica::Backend::gguf) ==
         "miguelamendez/mica-spark-x25-4b-gguf");
  assert(!spark.description.empty());
  assert(!spark.tags.empty());
  assert(mica::normalize_modality("tts") == "tts");
  assert(mica::modality_capability("text-to-text") == "text");
  assert(mica::modality_capability("img-text-to-text") == "vision");

  const auto custom_root = std::filesystem::temp_directory_path() /
                           ("mica-server-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(custom_root / "mica-server");
  {
    std::ofstream custom(custom_root / "mica-server/custom-models.json");
    custom << R"({"schema":1,"models":{"tiny-custom":{"id":"tiny-custom","source_repo":"owner/repo","source_url":"https://huggingface.co/owner/repo","modality":"text-to-text","capability":"text","license":"apache-2.0","description":"test","enabled":true,"variants":{"mlx":{"q4":{"status":"ready","artifact":"q4","reservation_gib":0.5}}}}}})";
  }
  auto with_custom = mica::load_registry(config);
  mica::merge_custom_models(with_custom, custom_root);
  assert(with_custom.model("tiny-custom").capability == "text");
  assert(with_custom.model("tiny-custom").artifacts.at(mica::Backend::mlx)
             .at(mica::Quantization::q4).supported);
  assert(with_custom.profile("all").models.back() == "tiny-custom");
  std::filesystem::remove_all(custom_root);

  const auto fits = mica::plan_startup(registry, all, mica::Backend::mlx,
                                       mica::Quantization::q4, 8.0);
  assert(!fits.error);
  assert(fits.admitted.size() == 4);
  assert(fits.admitted[0] == "spark-x25-4b");
  assert(fits.admitted[1] == "granite-speech-5");

  const auto baseline = mica::plan_startup(registry, all, mica::Backend::mlx,
                                           mica::Quantization::q4, 4.2);
  assert(!baseline.error);
  assert(baseline.admitted.size() == 2);
  assert(baseline.skipped.size() == 2);

  const auto too_small = mica::plan_startup(registry, all, mica::Backend::mlx,
                                            mica::Quantization::q4, 4.0);
  assert(too_small.error);

  std::vector<mica::ResidentModel> residents = {
      {"new-small", 1.0, 200, false, 0},
      {"busy", 5.0, 1, true, 1},
      {"expired-small", 1.0, 100, true, 0},
      {"expired-large", 3.0, 100, true, 0},
  };
  const auto ranked = mica::rank_eviction_candidates(std::move(residents));
  assert(ranked.size() == 3);
  assert(ranked[0].id == "expired-large");
  assert(ranked[1].id == "expired-small");
  assert(ranked[2].id == "new-small");

  std::cout << "scheduler tests passed\n";
  return 0;
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mica_server/update.hpp"
#include "mica_server/build_guard.hpp"
#include <nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using nlohmann::json;
namespace fs = std::filesystem;
int main() {
  const auto rejects = [](auto operation) {
    bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
  };
  const auto asset = [](const std::string& name) {
    return json{{"name", name}, {"size", 1024}, {"browser_download_url",
      "https://github.com/miguelamendez/mica-server/releases/download/v0.2.0-alpha.1/" + name}};
  };
  const std::string package = "mica-server-v0.2.0-alpha.1-linux-x86_64";
  json release = {{"tag_name", "v0.2.0-alpha.1"}, {"published_at", "2026-10-12T10:00:00Z"},
      {"draft", false}, {"prerelease", true}, {"assets", json::array({asset(package + ".tar.gz"), asset(package + ".tar.gz.sha256")})}};
  auto result = mica::select_release_candidate(json::array({release}), "linux", "x86_64");
  assert(result["method"] == "binary"); assert(result["prerelease"] == true);
  assert(mica::select_release_candidate(json::array({release}), "macos", "arm64")["method"] == "source");
  auto missing_checksum = release; missing_checksum["assets"].erase(1);
  assert(mica::select_release_candidate(json::array({missing_checksum}), "linux", "x86_64")["method"] == "source");
  assert(mica::select_release_candidate(json::array(), "linux", "x86_64")["method"] == "source");
  auto draft = release; draft["draft"] = true;
  assert(!mica::select_release_candidate(json::array({draft}), "linux", "x86_64").contains("tag"));
  auto newer = release; newer["tag_name"] = "v0.3.0"; newer["published_at"] = "2026-10-13T10:00:00Z"; newer["assets"] = json::array();
  assert(mica::select_release_candidate(json::array({release, newer}), "linux", "x86_64")["tag"] == "v0.3.0");
  auto bad = release; bad["tag_name"] = "../../outside";
  rejects([&] { mica::select_release_candidate(json::array({bad}), "linux", "x86_64"); });
  bad = release; bad["assets"][0]["browser_download_url"] = "https://example.org/wrong-owner.tar.gz";
  rejects([&] { mica::select_release_candidate(json::array({bad}), "linux", "x86_64"); });
  const std::string listing = package + "/\n" + package + "/bin/mica-server\n";
  mica::validate_update_archive_listing(listing, "drwxr-xr-x root root 0 dir\n-rwxr-xr-x root root 1024 binary\n", package);
  for (const auto& names : {std::string("/etc/passwd\n"), package + "/../outside\n", std::string("other/file\n"), std::string()})
    rejects([&] { mica::validate_update_archive_listing(names, "-rwxr-xr-x data\n", package); });
  for (const auto* types : {"lrwxr-xr-x symlink\n", "hrwxr-xr-x hardlink\n", "crwxr-xr-x device\n", "-rwsr-xr-x setuid\n"})
    rejects([&] { mica::validate_update_archive_listing(listing, types, package); });
  assert(mica::choose_build_budget({24, 20, false}, 8, 2, 10).memory_gib == 8);
  assert(mica::choose_build_budget({64, 50, false}, 32, 20, 32).memory_gib == 16);
  const auto limited = mica::choose_build_budget({24, 9, false}, 16, 2, 10);
  assert(limited.memory_gib <= 9 - limited.reserve_gib - 0.5);
  assert(limited.parallel_jobs == 1);
  rejects([] { mica::choose_build_budget({24, 5, false}, 8, 2, 10); });
  rejects([] { mica::choose_build_budget({24, 20, true}, 8, 2, 10); });
  rejects([] { mica::choose_build_budget({24, 25, false}, 8, 2, 10); });
  rejects([] { mica::run_build_guarded({}, {8, 4, 1}); });
  const auto memory = mica::live_memory_snapshot();
  assert(memory.total_gib > 0 && memory.available_gib >= 0 && memory.available_gib <= memory.total_gib);
  if (!memory.pressure_warning && memory.available_gib > 3) {
    const auto stopped = mica::run_build_guarded({"sh", "-c", "sleep 10"}, {0.000001, 2, 1}, true);
    assert(stopped.exit_code == 75); // Stop a tiny process, never allocate GBs in a test.
  } else std::cout << "Host pressure blocks the live process-group test; formula/security tests still run.\n";
  const auto root = fs::temp_directory_path() / ("mica-update-test-" + std::to_string(getpid()));
  assert(!fs::exists(root)); fs::create_directory(root);
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
  const auto payload = root / "payload", prefix = root / "prefix";
  fs::create_directories(payload / "bin"); fs::create_directories(payload / "share/mica-server/config");
  fs::create_directories(prefix / "bin"); fs::create_directories(prefix / "share/mica-server/config");
  std::ofstream(payload / "bin/mica-server") << "new binary";
  std::ofstream(payload / "share/mica-server/config/version") << "new data";
  std::ofstream(prefix / "bin/mica-server") << "old binary";
  std::ofstream(prefix / "share/mica-server/config/version") << "old data";
  mica::install_update_payload(payload, prefix, "test-transaction");
  assert(fs::exists(prefix / "bin/.mica-server.previous-test-transaction"));
  assert(fs::exists(prefix / "share/.mica-server.previous-test-transaction/config/version"));
  std::string line; { std::ifstream file(prefix / "bin/mica-server"); std::getline(file, line); }
  assert(line == "new binary");
  rejects([&] { mica::install_update_payload(payload, prefix, "../bad"); });
  fs::create_symlink("version", payload / "share/mica-server/config/link");
  rejects([&] { mica::install_update_payload(payload, prefix, "reject-link"); });
  rejects([&] { mica::update_command({"--channel", "invalid"}, prefix / "bin/mica-server", root, "0.1.0", "unknown"); });
  std::cout << "Update selection, archive safety, installation backup and memory admission passed.\n";
}

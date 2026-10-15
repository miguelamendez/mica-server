#include "mica_server/update.hpp"
#include "mica_server/build_guard.hpp"
#include "mica_server/command.hpp"
#include "mica_server/download.hpp"
#include "mica_server/hardware.hpp"
#include "mica_server/machine.hpp"
#include "mica_server/profiles.hpp"
#include "mica_server/service.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace mica {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr auto api = "https://api.github.com/repos/miguelamendez/mica-server";
constexpr auto repository = "https://github.com/miguelamendez/mica-server.git";
bool safe_name(const std::string& value) {
  return std::regex_match(value, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,127}"));
}
bool sha(const std::string& value) { return std::regex_match(value, std::regex("[0-9a-f]{40}")); }
json fetch(const std::string& endpoint) {
  const auto response = run_command({"curl", "--fail", "--silent", "--show-error", "--location",
      "--connect-timeout", "10", "--max-time", "30", "--retry", "1",
      "-H", "Accept: application/vnd.github+json", std::string(api) + endpoint}, true);
  if (response.exit_code) throw std::runtime_error("GitHub update check failed (network/rate limit): " + response.output);
  return json::parse(response.output);
}
void checked(const std::vector<std::string>& args) {
  const auto result = run_command(args, true);
  if (result.exit_code) throw std::runtime_error(display_command(args) + " failed: " + result.output.substr(0, 1000));
}
void guarded(const std::vector<std::string>& args, const ResolvedMachinePolicy& policy, int cores) {
  const auto budget = choose_build_budget(live_memory_snapshot(), policy.build_ram_gib, policy.build_parallel_jobs, cores);
  auto command = args;
  if (command.size() > 1 && command[1] == "--build")
    command.insert(command.end(), {"--parallel", std::to_string(budget.parallel_jobs)});
  std::cout << "Build admission: " << budget.memory_gib << " GiB cap, " << budget.reserve_gib
            << " GiB system reserve, " << budget.parallel_jobs << " jobs.\n";
  const auto result = run_build_guarded(command, budget);
  if (result.exit_code) throw std::runtime_error("source build stopped: " + result.output);
}
void ensure_plain_tree(const fs::path& path) {
  if (!fs::is_directory(path) || fs::is_symlink(path)) throw std::runtime_error("update payload directory is invalid");
  for (const auto& entry : fs::recursive_directory_iterator(path))
    if (entry.is_symlink() || !(entry.is_regular_file() || entry.is_directory()))
      throw std::runtime_error("update payload contains links or special files");
}
void validate_payload(const fs::path& payload, const fs::path& validation_root,
                      const std::string& revision, const std::string& tag) {
  ensure_plain_tree(payload);
  const auto binary = payload / "bin/mica-server";
  if (!fs::is_regular_file(binary) || access(binary.c_str(), X_OK))
    throw std::runtime_error("update lacks a runnable native binary");
  if (!fs::is_directory(payload / "share/mica-server/config") || !fs::is_directory(payload / "share/mica-server/schemas"))
    throw std::runtime_error("update lacks matching configuration and schemas");
  const auto identity = run_command({binary.string(), "--version"}, true);
  if (identity.exit_code || (identity.output.find(revision.substr(0, 12)) == std::string::npos &&
      (tag.empty() || identity.output.find(tag) == std::string::npos)))
    throw std::runtime_error("staged binary does not match the selected source revision/release tag");
  checked({binary.string(), "registry", "list", "--root", validation_root.string(),
           "--config-dir", (payload / "share/mica-server/config").string()});
}
}

nlohmann::json select_release_candidate(const json& input, const std::string& platform, const std::string& arch) {
  if (!input.is_array()) throw std::invalid_argument("release response must be an array");
  auto releases = input;
  std::sort(releases.begin(), releases.end(), [](const auto& a, const auto& b) {
    return a.value("published_at", "") > b.value("published_at", "");
  });
  for (const auto& release : releases) {
    if (release.value("draft", false) || release.value("published_at", "").empty()) continue;
    const auto tag = release.at("tag_name").get<std::string>();
    if (!safe_name(tag)) throw std::runtime_error("unsafe release tag");
    const auto package = "mica-server-" + tag + "-" + platform + "-" + arch;
    json result = {{"tag", tag}, {"package", package}, {"prerelease", release.value("prerelease", false)},
                   {"method", "source"}, {"reason", "Latest release has no matching archive and checksum; compile that release."}};
    json archive, checksum;
    for (const auto& asset : release.value("assets", json::array())) {
      if (asset.value("name", "") == package + ".tar.gz") archive = asset;
      if (asset.value("name", "") == package + ".tar.gz.sha256") checksum = asset;
    }
    if (!archive.is_null() && !checksum.is_null()) {
      if (archive.value("size", std::uint64_t{0}) == 0 || archive.value("size", std::uint64_t{0}) > 512ULL * 1024 * 1024 ||
          checksum.value("size", std::uint64_t{0}) == 0 || checksum.value("size", std::uint64_t{0}) > 65536)
        throw std::runtime_error("invalid/oversized release artifact metadata");
      const auto archive_url = archive.at("browser_download_url").get<std::string>();
      const auto checksum_url = checksum.at("browser_download_url").get<std::string>();
      const auto trusted = "https://github.com/miguelamendez/mica-server/releases/download/" + tag + "/";
      if (archive_url != trusted + package + ".tar.gz" || checksum_url != trusted + package + ".tar.gz.sha256")
        throw std::runtime_error("release download does not belong to the trusted repository");
      result["method"] = "binary"; result["reason"] = "Compatible release binary with SHA-256 checksum.";
      result["archive"] = archive; result["checksum"] = checksum;
    }
    return result;
  }
  return {{"method", "source"}, {"reason", "No published release; compile the pinned main revision."}};
}

void validate_update_archive_listing(const std::string& names, const std::string& details, const std::string& package) {
  if (!safe_name(package)) throw std::invalid_argument("invalid package name");
  std::istringstream paths(names); std::string name; bool any = false;
  while (std::getline(paths, name)) {
    const fs::path path(name);
    if (name.empty() || path.is_absolute() || !(name == package || name == package + "/" || name.starts_with(package + "/")))
      throw std::runtime_error("unsafe update archive path");
    for (const auto& part : path) if (part == ".." || part == ".") throw std::runtime_error("update archive traversal");
    any = true;
  }
  if (!any) throw std::runtime_error("empty update archive");
  std::istringstream rows(details);
  bool listed_details = false;
  while (std::getline(rows, name)) {
    listed_details = true;
    if (name.empty() || (name[0] != '-' && name[0] != 'd') || name.substr(0, 10).find_first_of("sS") != std::string::npos)
      throw std::runtime_error("update archive contains links, special files or setuid permissions");
  }
  if (!listed_details) throw std::runtime_error("empty detailed archive listing");
}

void install_update_payload(const fs::path& payload, const fs::path& prefix, const std::string& transaction) {
  if (!safe_name(transaction) || !prefix.is_absolute() || prefix == "/" || prefix == prefix.root_path())
    throw std::invalid_argument("unsafe update installation target");
  ensure_plain_tree(payload);
  const auto binary = prefix / "bin/mica-server", data = prefix / "share/mica-server";
  const auto new_binary = prefix / "bin" / (".mica-server.new-" + transaction);
  const auto new_data = prefix / "share" / (".mica-server.new-" + transaction);
  const auto old_binary = prefix / "bin" / (".mica-server.previous-" + transaction);
  const auto old_data = prefix / "share" / (".mica-server.previous-" + transaction);
  const bool has_client=fs::is_regular_file(payload/"bin/mica-dictation");
  const bool has_manager=fs::is_regular_file(payload/"bin/mica-client");
  const auto manager=prefix/"bin/mica-client",new_manager=prefix/"bin"/(".mica-client.new-"+transaction),old_manager=prefix/"bin"/(".mica-client.previous-"+transaction);
  if(has_manager&&(fs::is_symlink(manager)||(fs::exists(manager)&&!fs::is_regular_file(manager))))throw std::runtime_error("unexpected Mica Client installation target");
  const auto client=prefix/"bin/mica-dictation",new_client=prefix/"bin"/(".mica-dictation.new-"+transaction),old_client=prefix/"bin"/(".mica-dictation.previous-"+transaction);
  if(has_client&&(fs::is_symlink(client)||(fs::exists(client)&&!fs::is_regular_file(client))))throw std::runtime_error("unexpected dictation client installation target");
  if (fs::is_symlink(binary) || fs::is_symlink(data)) throw std::runtime_error("refusing to replace symlinked installation; select its real prefix");
  if ((fs::exists(binary) && !fs::is_regular_file(binary)) || (fs::exists(data) && !fs::is_directory(data)))
    throw std::runtime_error("existing installation has unexpected file types");
  fs::create_directories(binary.parent_path()); fs::create_directories(data.parent_path());
  for (const auto& path : {new_binary, new_data, old_binary, old_data,new_client,old_client,new_manager,old_manager})
    if (fs::exists(path)) throw std::runtime_error("update transaction path already exists");
  fs::copy_file(payload / "bin/mica-server", new_binary);
  fs::permissions(new_binary, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
  fs::copy(payload / "share/mica-server", new_data, fs::copy_options::recursive);
  if(has_client){fs::copy_file(payload/"bin/mica-dictation",new_client);fs::permissions(new_client,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);}
  if(has_manager){fs::copy_file(payload/"bin/mica-client",new_manager);fs::permissions(new_manager,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);}
  bool saved_binary = false, saved_data = false, installed_binary = false, installed_data = false;
  bool saved_client=false,installed_client=false;
  bool saved_manager=false,installed_manager=false;
  try {
    if (fs::exists(binary)) { fs::rename(binary, old_binary); saved_binary = true; }
    if (fs::exists(data)) { fs::rename(data, old_data); saved_data = true; }
    if(has_client&&fs::exists(client)){fs::rename(client,old_client);saved_client=true;}
    if(has_manager&&fs::exists(manager)){fs::rename(manager,old_manager);saved_manager=true;}
    fs::rename(new_data, data); installed_data = true;
    fs::rename(new_binary, binary); installed_binary = true;
    if(has_client){fs::rename(new_client,client);installed_client=true;}
    if(has_manager){fs::rename(new_manager,manager);installed_manager=true;}
  } catch (...) {
    // Move new files back to their transaction paths before restoring backups.
    if(installed_manager)fs::rename(manager,new_manager);
    if(saved_manager)fs::rename(old_manager,manager);
    if(installed_client)fs::rename(client,new_client);
    if(saved_client)fs::rename(old_client,client);
    if (installed_binary) fs::rename(binary, new_binary);
    if (installed_data) fs::rename(data, new_data);
    if (saved_data) fs::rename(old_data, data);
    if (saved_binary) fs::rename(old_binary, binary);
    throw;
  }
  // Previous binary and packaged data stay recoverable; never delete ~/.mica.
}

int update_command(const std::vector<std::string>& arguments, const fs::path& executable,
                   const fs::path& default_root, const std::string& version, const std::string& revision) {
  bool check_only = false, restart = false, force = false;
  auto root = default_root;
  auto prefix = executable.parent_path().filename() == "bin" ? executable.parent_path().parent_path() :
      fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".local";
  std::string channel = "release";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto& option = arguments[i];
    const auto value = [&]() { if (++i >= arguments.size()) throw std::invalid_argument("missing update option value"); return arguments[i]; };
    if (option == "--check") check_only = true;
    else if (option == "--restart") restart = true;
    else if (option == "--force") force = true;
    else if (option == "--channel") channel = value();
    else if (option == "--root") root = value();
    else if (option == "--prefix") prefix = value();
    else throw std::invalid_argument("unknown update option: " + option);
  }
  if (channel != "release" && channel != "source") throw std::invalid_argument("update channel must be release or source");
  if (!prefix.is_absolute() || prefix == "/") throw std::invalid_argument("update prefix must be an absolute installation prefix");
  const auto hardware = detect_hardware();
  const auto platform = hardware.os == "wsl" ? "linux" : hardware.os;
  const auto arch = hardware.arch == "aarch64" ? "arm64" : hardware.arch;
  if ((platform != "linux" && platform != "macos") || (arch != "arm64" && arch != "x86_64"))
    throw std::runtime_error("self-update supports macOS/Linux/WSL ARM64 and x86-64");
  json candidate = channel == "release" ? select_release_candidate(fetch("/releases?per_page=30"), platform, arch) :
      json{{"method", "source"}, {"reason", "Latest pinned main source revision."}};
  const auto reference = candidate.value("tag", std::string("main"));
  const auto target = fetch("/commits/" + reference).at("sha").get<std::string>();
  if (!sha(target)) throw std::runtime_error("invalid pinned update revision");
  candidate["target_revision"] = target; candidate["channel"] = channel;
  candidate["current_version"] = version; candidate["current_revision"] = revision;
  candidate["prefix"] = prefix.string(); candidate["schema"] = 1;
  candidate["checked_at_unix"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::string base = revision;
  const bool dirty = base.ends_with("-dirty"); if (dirty) base.resize(base.size() - 6);
  const auto suffix = base.rfind("-g"); if (suffix != std::string::npos) base = base.substr(suffix + 2);
  bool available = !target.starts_with(base);
  if (available && base != "unknown" && safe_name(base)) {
    const auto comparison = fetch("/compare/" + base + "..." + target).at("status").get<std::string>();
    available = comparison == "ahead";
    if (comparison == "behind") candidate["reason"] = "Installed source is newer than the selected release; no downgrade.";
    if (comparison == "diverged") throw std::runtime_error("update target diverges from installed revision; refusing automatic replacement");
  }
  candidate["available"] = available; candidate["local_changes"] = dirty;
  const auto memory = live_memory_snapshot();
  candidate["memory"] = {{"total_gib", memory.total_gib}, {"available_gib", memory.available_gib},
                          {"used_or_unavailable_gib", memory.total_gib - memory.available_gib}, {"pressure_warning", memory.pressure_warning}};
  try {
    const auto policy = resolve_machine_policy(hardware, fs::exists(root / "config/machine.yaml") ? load_machine_policy(root / "config/machine.yaml") : MachinePolicy{});
    const auto budget = choose_build_budget(memory, policy.build_ram_gib, policy.build_parallel_jobs, hardware.logical_cpu_cores);
    candidate["source_build"] = {{"eligible", true}, {"memory_cap_gib", budget.memory_gib},
                                  {"system_reserve_gib", budget.reserve_gib}, {"parallel_jobs", budget.parallel_jobs}};
  } catch (const std::exception& error) { candidate["source_build"] = {{"eligible", false}, {"reason", error.what()}}; }
  fs::create_directories(root / "state");
  const auto cache_temp = root / "state" / (".update-check-" + std::to_string(getpid()) + ".json");
  { std::ofstream cache(cache_temp); cache << candidate.dump(2) << '\n'; if (!cache) throw std::runtime_error("cannot save update-check metadata"); }
  fs::rename(cache_temp, root / "state/update-check.json");
  if (check_only || (!available && !force)) { std::cout << candidate.dump(2) << '\n'; return 0; }
  if (dirty && !force) throw std::runtime_error("installed build contains local changes; --force is required to replace it");
  if (local_server_running(root) && !restart) throw std::runtime_error("Mica is running; update --restart or stop it first");
  fs::create_directories(root / "updates");
  const int lock = open((root / "updates/update.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { if (lock >= 0) close(lock); throw std::runtime_error("another update is running"); }
  struct Unlock { int fd; ~Unlock() { close(fd); } } unlock{lock};
  const auto transaction = std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto scratch = root / "updates" / transaction;
  fs::create_directories(scratch); fs::permissions(scratch, fs::perms::owner_all);
  fs::path payload;
  if (candidate.at("method") == "binary") {
    const auto archive = scratch / "release.tar.gz", checksum = scratch / "release.sha256";
    download_native_file(candidate["archive"]["browser_download_url"], archive, candidate["archive"].value("size", std::uint64_t{0}), 1);
    download_native_file(candidate["checksum"]["browser_download_url"], checksum, candidate["checksum"].value("size", std::uint64_t{0}), 1);
    std::ifstream checksum_input(checksum); std::string expected; checksum_input >> expected;
    if (!std::regex_match(expected, std::regex("[0-9a-fA-F]{64}"))) throw std::runtime_error("invalid release checksum");
    auto hash = run_command(platform == "macos" ? std::vector<std::string>{"shasum", "-a", "256", archive.string()} :
        std::vector<std::string>{"sha256sum", archive.string()}, true);
    std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return std::tolower(c); });
    if (hash.exit_code || !hash.output.starts_with(expected)) throw std::runtime_error("release SHA-256 verification failed; aborting");
    const auto names = run_command({"tar", "-tzf", archive.string()}, true);
    const auto details = run_command({"tar", "-tvzf", archive.string()}, true);
    if (names.exit_code || details.exit_code) throw std::runtime_error("cannot inspect release archive");
    validate_update_archive_listing(names.output, details.output, candidate.at("package"));
    checked({"tar", "-xzf", archive.string(), "-C", scratch.string()});
    payload = scratch / candidate.at("package").get<std::string>();
    const auto runnable = run_command({(payload / "bin/mica-server").string(), "--version"}, true);
    if (runnable.exit_code) { candidate["method"] = "source"; candidate["reason"] = "Release binary cannot run on this host ABI; compiling the same revision."; }
  }
  if (candidate.at("method") == "source") {
    const auto policy = resolve_machine_policy(hardware, fs::exists(root / "config/machine.yaml") ?
        load_machine_policy(root / "config/machine.yaml") : MachinePolicy{});
    (void)choose_build_budget(live_memory_snapshot(), policy.build_ram_gib, policy.build_parallel_jobs, hardware.logical_cpu_cores);
    for (const auto* dependency : {"git", "cmake", "c++"})
      if (!command_exists(dependency)) throw std::runtime_error(std::string("source fallback needs ") + dependency + "; install source-build prerequisites");
    const auto source = scratch / "source", build = scratch / "build"; payload = scratch / "payload";
    checked({"git", "clone", "--filter=blob:none", "--no-checkout", "--depth", "1", repository, source.string()});
    checked({"git", "-C", source.string(), "fetch", "--depth", "1", "origin", target});
    checked({"git", "-C", source.string(), "checkout", "--detach", target});
    guarded({"cmake", "-S", source.string(), "-B", build.string(), "-DCMAKE_BUILD_TYPE=Release", "-DMICA_SERVER_BUILD_TESTS=OFF"}, policy, hardware.logical_cpu_cores);
    guarded({"cmake", "--build", build.string(), "--config", "Release"}, policy, hardware.logical_cpu_cores);
    checked({"cmake", "--install", build.string(), "--prefix", payload.string()});
  }
  validate_payload(payload, scratch / "validation-home", target, candidate.value("tag", ""));
  // Ensure the installation prefix is writable before stopping a live server.
  fs::create_directories(prefix / "bin"); fs::create_directories(prefix / "share");
  if (access((prefix / "bin").c_str(), W_OK) || access((prefix / "share").c_str(), W_OK))
    throw std::runtime_error("installation prefix is not writable; use a user-owned --prefix");
  const bool running = local_server_running(root);
  const auto active = running ? local_server_status(root).value("profile", json::object()).value("name", "") : "";
  const bool dashboard = running && local_server_status(root).value("dashboard", false);
  if (running) {
    if (!restart) throw std::runtime_error("server started during update preparation; retry with --restart");
    service_command("stop", {"--root", root.string()}, executable, root);
  }
  try { install_update_payload(payload, prefix, transaction); }
  catch (...) {
    if (running) { try { service_command("start", {"--root", root.string(), "--workload", active}, executable, root); } catch (...) {} }
    throw;
  }
  const auto installed = prefix / "bin/mica-server";
  if (running) {
    std::vector<std::string> args{installed.string(), "start", "--root", root.string()};
    if (!active.empty()) args.insert(args.end(), {"--workload", active});
    if (dashboard) args.push_back("--dashboard");
    const auto result = run_command(args, true);
    if (result.exit_code) throw std::runtime_error("Update installed but workload restart failed. Previous files remain in .mica-server.previous-" + transaction + "; " + result.output.substr(0, 1000));
  }
  candidate["installed"] = true; candidate["restarted"] = running;
  candidate["transaction"] = transaction;
  std::cout << candidate.dump(2) << '\n';
  return 0;
}
}

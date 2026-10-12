#include "mica_server/credentials.hpp"
#include "mica_server/setup.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mica {
namespace {
bool valid(const std::string& key) {
  return key.size() >= 16 && key.size() <= 512 && std::all_of(key.begin(), key.end(),
      [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool equal(const std::string& a, const std::string& b) {
  unsigned int difference = static_cast<unsigned int>(a.size() ^ b.size());
  for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
    difference |= (i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0);
  return difference == 0;
}
}
void write_private_file(const std::filesystem::path& path, const std::string& contents) {
  if (std::filesystem::is_symlink(path)) throw std::runtime_error("private file cannot be a symlink");
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".pending-" + generate_api_key().substr(0, 16);
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot create private file");
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto n = write(fd, contents.data() + offset, contents.size() - offset);
    if (n <= 0) { close(fd); std::filesystem::remove(temporary); throw std::runtime_error("private file write failed"); }
    offset += n;
  }
  if (fsync(fd) != 0) { close(fd); std::filesystem::remove(temporary); throw std::runtime_error("private file sync failed"); }
  close(fd);
  try { std::filesystem::rename(temporary, path); }
  catch (...) { std::filesystem::remove(temporary); throw; }
}
Credentials::Credentials(std::string key, std::filesystem::path file,
                         std::filesystem::path admin_file, bool separate, std::filesystem::path server_config)
    : inference_(std::move(key)), inference_file_(std::move(file)),
      admin_file_(std::move(admin_file)), separate_(separate) {
  if (!valid(inference_)) throw std::runtime_error("invalid inference API key");
  if (separate_) {
    if (std::filesystem::is_symlink(admin_file_)) throw std::runtime_error("admin key cannot be a symlink");
    if (!std::filesystem::exists(admin_file_)) write_private_file(admin_file_, generate_api_key() + "\n");
    std::ifstream stream(admin_file_); std::getline(stream, admin_);
    if (!admin_.empty() && admin_.back() == '\r') admin_.pop_back();
    if (!valid(admin_) || equal(admin_, inference_)) throw std::runtime_error("admin and inference keys must be valid and distinct");
    chmod(admin_file_.c_str(), 0600);
    if (!server_config.empty()) {
      auto config = nlohmann::json::object();
      if (std::filesystem::exists(server_config)) { std::ifstream file(server_config); config = nlohmann::json::parse(file); }
      // Persist file references, never inline credentials. Otherwise restarting
      // after rotation could resurrect a revoked inline key from old settings.
      config.erase("api_key"); config["api_key_file"] = inference_file_.string();
      config["admin_api_key_file"] = admin_file_.string(); config["schema"] = 1;
      if (config.contains("ui") && config["ui"].value("api_key", "") == inference_) {
        config["ui"].erase("api_key"); config["ui"]["api_key_file"] = inference_file_.string();
      }
      write_private_file(server_config, config.dump(2));
    }
  }
}
bool Credentials::allows(const std::string& authorization, bool administrative) const {
  if (!authorization.starts_with("Bearer ")) return false;
  const auto key = authorization.substr(7);
  if (!valid(key)) return false;
  std::lock_guard lock(mutex_);
  if (separate_) return equal(key, admin_) || (!administrative && equal(key, inference_));
  return equal(key, inference_);
}
std::string Credentials::rotate(const std::string& kind) {
  if (kind != "admin" && kind != "inference") throw std::invalid_argument("kind must be admin or inference");
  std::lock_guard lock(mutex_);
  if (kind == "admin" && !separate_) throw std::invalid_argument("enable separate administration first");
  const auto key = generate_api_key();
  const auto& path = kind == "admin" ? admin_file_ : inference_file_;
  if (path.empty()) throw std::invalid_argument("rotation needs a persistent key file");
  write_private_file(path, key + "\n");
  (kind == "admin" ? admin_ : inference_) = key;
  return key;
}
}

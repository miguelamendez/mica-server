#pragma once
#include <filesystem>
#include <mutex>
#include <string>

namespace mica {
// Credentials are intentionally independent of HTTP/UI and shared by the
// native management clients. Private files are never returned by inventory.
class Credentials {
 public:
  Credentials(std::string inference_key, std::filesystem::path inference_file,
              std::filesystem::path admin_file, bool separate_admin,
              std::filesystem::path server_config = {});
  bool allows(const std::string& authorization, bool administrative) const;
  std::string rotate(const std::string& kind);
  bool separated() const { return separate_; }
  const std::filesystem::path& inference_file() const { return inference_file_; }
  const std::filesystem::path& admin_file() const { return admin_file_; }
 private:
  mutable std::mutex mutex_;
  std::string inference_, admin_;
  std::filesystem::path inference_file_, admin_file_;
  bool separate_;
};
void write_private_file(const std::filesystem::path& path, const std::string& contents);
}

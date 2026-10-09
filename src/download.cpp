#include "mica_server/download.hpp"
#include "mica_server/command.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <stdexcept>
#include <vector>

namespace mica {
void download_native_file(const std::string& url, const std::filesystem::path& destination,
                          std::uint64_t size_bytes, int parts, std::uint64_t parallel_threshold_bytes) {
  if (parts < 1 || parts > 4) throw std::invalid_argument("download concurrency must be 1..4");
  if (!(url.starts_with("https://") || url.starts_with("http://")))
    throw std::invalid_argument("download requires HTTP(S)");
  std::filesystem::create_directories(destination.parent_path());
  const auto fetch = [&](const std::filesystem::path& output, const std::string& range) {
    std::vector<std::string> args = {"curl", "--location", "--fail", "--silent", "--show-error",
        "--connect-timeout", "20", "--speed-limit", "1024", "--speed-time", "60",
        "--retry", "3", "--output", output.string()};
    if (!range.empty()) args.insert(args.end(), {"--range", range});
    args.push_back(url);
    const auto result = run_command(args, true);
    if (result.exit_code != 0) throw std::runtime_error("native download failed: " + result.output);
  };
  // Small files, unknown sizes, or a requested single connection use one stream.
  if (size_bytes == 0 || size_bytes < static_cast<std::uint64_t>(parts) || size_bytes < parallel_threshold_bytes || parts == 1) {
    fetch(destination, "");
    if (size_bytes && std::filesystem::file_size(destination) != size_bytes)
      throw std::runtime_error("download size mismatch");
    return;
  }
  std::vector<std::filesystem::path> paths;
  std::vector<std::future<void>> transfers;
  const auto stride = (size_bytes + parts - 1) / parts;
  for (int i = 0; i < parts; ++i) {
    const auto start = stride * i;
    const auto end = std::min(size_bytes, start + stride) - 1;
    const auto path = std::filesystem::path(destination.string() + ".range-" + std::to_string(i));
    paths.push_back(path);
    transfers.push_back(std::async(std::launch::async, [&, path, start, end] {
      fetch(path, std::to_string(start) + "-" + std::to_string(end));
      if (std::filesystem::file_size(path) != end - start + 1)
        throw std::runtime_error("server did not return the requested byte range");
    }));
  }
  std::exception_ptr failure;
  // Join all transfers before cleanup, including after a failure.
  for (auto& transfer : transfers) try { transfer.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
  try {
    if (failure) std::rethrow_exception(failure);
    std::ofstream assembled(destination, std::ios::binary | std::ios::trunc);
    if (!assembled) throw std::runtime_error("cannot assemble downloaded file");
    std::array<char, 65536> buffer;
    for (const auto& path : paths) {
      std::ifstream part(path, std::ios::binary);
      if (!part) throw std::runtime_error("cannot read downloaded range");
      while (part.read(buffer.data(), buffer.size()) || part.gcount()) {
        assembled.write(buffer.data(), part.gcount());
        if (!assembled) throw std::runtime_error("cannot write downloaded file");
      }
      if (part.bad()) throw std::runtime_error("cannot read downloaded range");
    }
    assembled.close();
    if (!assembled || std::filesystem::file_size(destination) != size_bytes)
      throw std::runtime_error("assembled download size mismatch");
  } catch (...) {
    for (const auto& path : paths) std::filesystem::remove(path);
    std::filesystem::remove(destination);
    throw;
  }
  for (const auto& path : paths) std::filesystem::remove(path);
}
}

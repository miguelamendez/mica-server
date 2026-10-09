#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace mica {
// Stream disjoint ranges to disk. No model-sized memory buffer. The caller
// verifies SHA-256 before publishing the assembled file. Throws on failure.
void download_native_file(const std::string& url,
                          const std::filesystem::path& destination,
                          std::uint64_t size_bytes, int parts = 4,
                          std::uint64_t parallel_threshold_bytes = 512ULL * 1024 * 1024);
}

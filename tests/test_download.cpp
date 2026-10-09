#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mica_server/download.hpp"
#include <httplib.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

int main() {
  char directory[] = "/tmp/mica-download-test-XXXXXX";
  const auto root = std::filesystem::path(mkdtemp(directory));
  httplib::Server server;
  std::string data;
  for (int i = 0; i < 10003; ++i) data.push_back(static_cast<char>('a' + i % 26));
  server.Get("/model", [&](const auto&, auto& response) { response.set_content(data, "application/octet-stream"); });
  const int port = server.bind_to_any_port("127.0.0.1");
  assert(port > 0);
  std::thread listener([&] { server.listen_after_bind(); });
  const auto url = "http://127.0.0.1:" + std::to_string(port) + "/model";
  try {
    for (int parts : {1, 2, 4}) {
      const auto output = root / (std::to_string(parts) + ".bin");
      mica::download_native_file(url, output, data.size(), parts, 1);
      std::ifstream file(output, std::ios::binary);
      const std::string actual((std::istreambuf_iterator<char>(file)), {});
      assert(actual == data);
    }
    bool rejected = false;
    try { mica::download_native_file(url, root / "invalid.bin", data.size(), 8, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { mica::download_native_file(url + "-absent", root / "missing.bin", data.size(), 4, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    for (const auto& entry : std::filesystem::directory_iterator(root))
      assert(entry.path().filename().string().find(".range-") == std::string::npos);
  } catch (...) {
    server.stop(); listener.join(); std::filesystem::remove_all(root); throw;
  }
  server.stop(); listener.join(); std::filesystem::remove_all(root);
  std::cout << "Single and parallel byte-range downloads, uneven ranges and failure cleanup passed.\n";
}

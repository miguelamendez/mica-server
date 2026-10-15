#pragma once
#include <filesystem>
#include <string>
#include <httplib.h>
namespace mica {
// Caller authenticates and verifies same-origin access before entering.
void serve_app_http(const httplib::Request& request,httplib::Response& response,
                    const std::filesystem::path& root,const std::string& id);
}

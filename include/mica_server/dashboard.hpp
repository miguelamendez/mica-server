#pragma once
#include <functional>
#include <memory>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "mica_server/server.hpp"
#include "mica_server/credentials.hpp"
namespace mica {
struct DashboardHost {
  std::function<nlohmann::json()> status;
  std::function<void()> pause;
  std::function<void()> resume;
  std::function<void()> reload;
};
void register_dashboard(httplib::Server& server, const ServerOptions& options,
                        std::shared_ptr<Credentials> credentials, DashboardHost host);
int dashboard_job_command(const std::vector<std::string>& arguments);
}

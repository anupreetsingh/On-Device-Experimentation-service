#pragma once

#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "activation.h"
#include "assigner.h"
#include "event_loop.h"
#include "exposure_log.h"
#include "files.h"
#include "server.h"
#include "uds_client.h"

namespace expd {

constexpr char kTestDevice[] = "6F1C2B7A-3D4E-4F50-9A6B-7C8D9E0F1A2B";

// expd's pieces wired the way main.cpp wires them, with the event loop on a background
// thread. The loop's thread is the only one touching them until the destructor joins it.
class TestDaemon {
 public:
  TestDaemon(const std::string& dir, const std::string& config,
             std::chrono::milliseconds flush_interval = std::chrono::seconds(60))
      : dir_(dir) {
    std::string error;
    if (!config.empty()) write_file_atomically(dir + "/config", config, 0600, &error);
    assigner_ = std::make_unique<Assigner>(dir, kTestDevice);
    loaded_ = assigner_->load(&error);
    ExposureLog::Options options;
    options.path = dir + "/exposures.log";
    options.interval = flush_interval;
    options.leeway = flush_interval / 10;
    exposures_ = std::make_unique<ExposureLog>(options);
    exposures_->start(&error);
    server_ = std::make_unique<Server>(loop_, *assigner_, exposures_.get());
    int fd = listen_unix(socket_path(), &error);
    if (fd >= 0) server_->listen_on(fd);
    thread_ = std::thread([this] { loop_.run(); });
  }

  ~TestDaemon() {
    stop();
  }

  // Stops the loop, then flushes and stops the exposure log, like expd on SIGTERM.
  void stop() {
    if (!thread_.joinable()) return;
    loop_.stop();
    thread_.join();
    server_.reset();
    exposures_->stop();
  }

  bool loaded() const { return loaded_; }
  std::string socket_path() const { return dir_ + "/expd.sock"; }

  // One request on a fresh connection. "" if anything fails.
  std::string call(std::string_view request) const {
    int fd = uds::connect_to(socket_path());
    if (fd < 0) return "";
    std::string reply;
    bool ok = uds::request(fd, request, &reply);
    close(fd);
    return ok ? reply : "";
  }

 private:
  std::string dir_;
  EventLoop loop_;
  std::unique_ptr<Assigner> assigner_;
  std::unique_ptr<ExposureLog> exposures_;
  std::unique_ptr<Server> server_;
  bool loaded_ = false;
  std::thread thread_;
};

}  // namespace expd

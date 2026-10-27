#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace rmtc {
struct AiConfig {
  std::string url, token, model;
  long timeout_ms = 1800;
  int max_tokens = 2200;
};

class AiWorker {
public:
  struct Result {
    std::string session, source, reply, status;
    std::uint64_t generation = 0;
    bool http_success = false, stop = false;
  };
  explicit AiWorker(AiConfig config);
  ~AiWorker();
  AiWorker(const AiWorker &) = delete;
  AiWorker &operator=(const AiWorker &) = delete;
  void reset(const std::string &session);
  void observe(const nlohmann::json &perception, const nlohmann::json &known_events);
  std::optional<Result> poll();
  void acknowledge(const Result &result, bool understood, bool partial = false);
  std::string status() const;
  bool enabled() const { return enabled_; }
  bool observed_http_success() const;

private:
  struct Job {
    std::string session, source, body;
    std::uint64_t generation;
  };
  Result perform(const Job &job);
  void run();
  AiConfig config_;
  bool enabled_ = false, stopping_ = false, busy_ = false, disabled_ = false;
  bool http_success_ = false, understood_ = false;
  unsigned failures_ = 0, invalid_replies_ = 0;
  std::string session_, understood_source_, status_ = "disabled";
  std::vector<std::string> announcements_;
  std::set<std::string> seen_;
  std::optional<Job> pending_;
  std::optional<Result> ready_;
  std::chrono::steady_clock::time_point retry_after_{};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<std::uint64_t> generation_{0};
  std::thread thread_;
};
}  // namespace rmtc

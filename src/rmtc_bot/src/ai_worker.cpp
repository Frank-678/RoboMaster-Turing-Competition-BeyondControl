#include "rmtc_bot/ai_worker.hpp"
#include "rmtc_bot/events.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <memory>
#include <stdexcept>

namespace rmtc {
namespace {
using Json = nlohmann::json;
struct CurlRuntime {
  CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    throw std::runtime_error("curl initialization failed"); }
  ~CurlRuntime() { curl_global_cleanup(); }
};
struct Download {
  std::string body;
  static std::size_t append(char *data, std::size_t size, std::size_t count, void *opaque) {
    auto &self = *static_cast<Download *>(opaque);
    const auto bytes = size * count;
    if (bytes > 262144 || self.body.size() > 262144 - bytes) return 0;
    try { self.body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
  }
};
struct Cancellation {
  const std::atomic<std::uint64_t> &generation;
  std::uint64_t expected;
  static int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto &self = *static_cast<Cancellation *>(opaque);
    return self.generation.load() != self.expected;
  }
};
}  // namespace

AiWorker::AiWorker(AiConfig config) : config_(std::move(config)) {
  enabled_ = !config_.url.empty() && !config_.token.empty();
  if (!enabled_) return;
  if ((config_.url.rfind("http://", 0) != 0 && config_.url.rfind("https://", 0) != 0) ||
      config_.token.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("invalid AI URL or token format");
  if (config_.timeout_ms < 100 || config_.timeout_ms > 30000 ||
      config_.max_tokens < 1 || config_.max_tokens > 8192)
    throw std::runtime_error("AI timeout/max_tokens must be configured explicitly");
  static CurlRuntime runtime;
  (void)runtime;
  status_ = "ready";
  thread_ = std::thread([this] { run(); });
}

AiWorker::~AiWorker() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; ++generation_; }
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void AiWorker::reset(const std::string &session) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;  // 旧请求由 curl 进度回调中止，旧结果也不能进入新场次。
  session_ = session;
  pending_.reset(); ready_.reset();
  announcements_.clear(); seen_.clear(); understood_source_.clear();
  disabled_ = false; http_success_ = false; understood_ = false;
  failures_ = invalid_replies_ = 0;
  retry_after_ = {};
  status_ = enabled_ ? "ready" : "disabled";
}

void AiWorker::observe(const Json &p, const Json &known_events) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (disabled_ || p.at("session_id") != session_) return;
  const auto text = p.value("event_text", std::string());
  if (text.size() > 65536) { status_ = "announcement_too_large"; return; }
  if (!text.empty() && seen_.insert(text).second) {
    announcements_.push_back(text);
    invalid_replies_ = 0;
  }
  std::string source;
  for (const auto &message : announcements_) {
    if (!source.empty()) source += '\n';
    source += message;
  }
  // 超限不截断公告制造语义；仍维持本地决策，日志明确显示不可解析。
  if (source.size() > 131072) { status_ = "announcement_history_too_large"; return; }
  if (busy_ || pending_ || ready_ || std::chrono::steady_clock::now() < retry_after_ ||
      invalid_replies_ >= 3 || (understood_ && source == understood_source_)) return;
  Json context = {{"session_id", session_}, {"tick", p.at("tick")}, {"map", p.at("map")},
                  {"announcements", source}, {"known_events", known_events}};
  Json body = {{"messages", Json::array({{{"role", "system"}, {"content", event_system_prompt()}},
                                          {{"role", "user"}, {"content", context.dump()}}})},
               {"max_tokens", config_.max_tokens}, {"stream", false}};
  if (!config_.model.empty()) body["model"] = config_.model;
  const auto serialized = body.dump();
  if (serialized.size() > 2097152) { status_ = "request_too_large"; return; }
  pending_ = Job{session_, source, serialized, generation_.load()};
  status_ = "queued";
  wake_.notify_one();
}

std::optional<AiWorker::Result> AiWorker::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = std::move(ready_);
  ready_.reset();
  return result;
}

void AiWorker::acknowledge(const Result &r, bool understood, bool partial) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (r.generation != generation_.load() || r.session != session_) return;
  http_success_ = http_success_ || r.http_success;
  disabled_ = r.stop;
  status_ = r.status;
  if (understood && !partial) {
    understood_ = true; understood_source_ = r.source;
    failures_ = invalid_replies_ = 0;
    status_ = "events_validated";
  } else {
    if (r.http_success) {
      ++invalid_replies_;
      status_ = partial ? "events_incomplete" : "invalid_model_reply";
    }
    failures_ = std::min(failures_ + 1, 4U);
    retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1U << (failures_ - 1));
  }
}

std::string AiWorker::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}
bool AiWorker::observed_http_success() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return http_success_;
}

AiWorker::Result AiWorker::perform(const Job &job) {
  Result result;
  result.session = job.session; result.source = job.source; result.generation = job.generation;
  result.status = "network_error";
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) return result;
  curl_slist *raw_headers = nullptr;
  raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
  raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + config_.token).c_str());
  raw_headers = curl_slist_append(raw_headers, "Expect:");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
  Download response;
  Cancellation cancel{generation_, job.generation};
  curl_easy_setopt(curl.get(), CURLOPT_URL, config_.url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, job.body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(job.body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, config_.timeout_ms);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, std::min(500L, config_.timeout_ms));
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &Download::append);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &Cancellation::progress);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancel);
  const auto code = curl_easy_perform(curl.get());
  if (code != CURLE_OK) return result;
  long status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  result.http_success = status >= 200 && status < 300;
  result.status = "http_" + std::to_string(status);
  if (status == 401 || status == 403 || status == 400 || status == 404) result.stop = true;
  try {
    const auto root = Json::parse(response.body);
    if (result.http_success) result.reply = root.at("choices").at(0).at("message").at("content").get<std::string>();
    else if (status == 429 && root.at("error").value("code", std::string()) == "quota_exceeded") {
      result.stop = true; result.status = "quota_exceeded";
    }
  } catch (const Json::exception &) { /* 非 JSON 错误正文也走有界退避。 */ }
  return result;
}

void AiWorker::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
      if (stopping_) return;
      job = std::move(*pending_); pending_.reset(); busy_ = true;
    }
    Result result;
    try { result = perform(job); }
    catch (...) {
      result.session = job.session; result.source = job.source;
      result.generation = job.generation; result.status = "request_failed";
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
      if (job.generation == generation_.load() && job.session == session_) ready_ = std::move(result);
    }
  }
}
}  // namespace rmtc

#include "rmtc_bot/ai_worker.hpp"
#include "rmtc_bot/events.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <memory>
#include <stdexcept>

// 本文件只管“公告→HTTP请求→结果”，不等待ROS，也不直接选择机器人动作。
namespace rmtc {
namespace { // curl回调和资源包装只在本文件可见。
using Json = nlohmann::json; // 请求体和服务响应的JSON类型。
// 进程级curl初始化/清理；函数内static保证只初始化一次。
struct CurlRuntime {
  CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    throw std::runtime_error("curl initialization failed"); }
  ~CurlRuntime() { curl_global_cleanup(); }
};
struct Download {
  std::string body; // 已收到的HTTP正文，最多256KiB。
  // data是新收到的数据；size*count是字节数；opaque指向当前Download。
  static std::size_t append(char *data, std::size_t size, std::size_t count, void *opaque) {
    auto &self = *static_cast<Download *>(opaque); // 取回本次请求的接收缓冲。
    const auto bytes = size * count; // 本次回调给出的正文长度。
    if (bytes > 262144 || self.body.size() > 262144 - bytes) return 0;
    try { self.body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
  }
};
struct Cancellation {
  const std::atomic<std::uint64_t> &generation; // 当前场次代数的只读引用。
  std::uint64_t expected; // 本次请求创建时的代数。
  // curl定期调用；返回非0取消请求。四个curl_off_t进度参数在这里不用。
  static int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto &self = *static_cast<Cancellation *>(opaque); // opaque是perform传入的取消上下文。
    return self.generation.load() != self.expected;
  }
};
}  // namespace

AiWorker::AiWorker(AiConfig config) : config_(std::move(config)) {
  // 配置复制进本对象后固定；只检查格式和范围，不替用户补URL、令牌或模型名。
  enabled_ = !config_.url.empty() && !config_.token.empty();
  if (!enabled_) {
    status_ = config_.url.empty() ? (config_.token.empty() ? "disabled_missing_url_and_token" : "disabled_missing_url")
                                : "disabled_missing_token";
    return;
  }
  if ((config_.url.rfind("http://", 0) != 0 && config_.url.rfind("https://", 0) != 0) ||
      config_.token.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("invalid AI URL or token format");
  if (config_.model.empty()) throw std::runtime_error("AI model must be configured explicitly");
  if (config_.timeout_ms < 100 || config_.timeout_ms > 60000 ||
      config_.connect_timeout_ms < 100 || config_.connect_timeout_ms > config_.timeout_ms ||
      config_.max_tokens < 1 || config_.max_tokens > 8192)
    throw std::runtime_error("AI timeout/max_tokens must be configured explicitly");
  if (!config_.thinking.empty() && config_.thinking != "enabled" && config_.thinking != "disabled")
    throw std::runtime_error("AI thinking must be enabled, disabled or empty");
  static CurlRuntime runtime; // 第一个工作线程出现前初始化curl，进程退出时统一清理。
  (void)runtime;
  status_ = "ready";
  thread_ = std::thread([this] { run(); });
}

AiWorker::~AiWorker() {
  // lock保护共享退出标志；generation让正在联网的旧请求尽快停止。
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; ++generation_; } // 加锁标记退出，换代使在途请求取消。
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void AiWorker::reset(const std::string &session) {
  // 仅清本场状态；已有后台线程继续复用。旧请求完成后也会被代数检查丢弃。
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  ++generation_;  // 旧请求由 curl 进度回调中止，旧结果也不能进入新场次。
  session_ = session;
  pending_.reset(); ready_.reset();
  announcements_.clear(); seen_.clear(); understood_source_.clear();
  disabled_ = false; http_success_ = false; understood_ = false;
  failures_ = invalid_replies_ = 0;
  started_ = finished_ = successes_ = 0;
  last_http_ = last_curl_ = 0; known_mock_ = false;
  last_elapsed_ms_ = last_connect_ms_ = last_first_byte_ms_ = 0; last_result_.clear();
  retry_after_ = {};
  if (enabled_) status_ = "ready";
}

void AiWorker::observe(const Json &p, const Json &known_events) {
  // p是最新公开感知，known_events是本地已经校验过的事件摘要。
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  if (disabled_ || p.at("session_id") != session_) return;
  const auto text = p.value("event_text", std::string()); // 这帧携带的近期公告批次，可能与上一帧重复。
  if (text.size() > 65536) { status_ = "announcement_too_large"; return; }
  if (!text.empty() && seen_.insert(text).second) {
    // 感知经常携带“旧公告+新公告”。新批次已原样包含旧批次时，移除旧副本。
    // 只比较原文字节，不解释事件；否则100批累积文本会膨胀成约50倍上下文。
    announcements_.erase(std::remove_if(announcements_.begin(), announcements_.end(),
        [&text](const std::string &previous) { return text.find(previous) != std::string::npos; }),
        announcements_.end()); // previous是已保存的一批完整原文；未被包含的旧公告继续保留。
    announcements_.push_back(text);
    invalid_replies_ = 0;
  }
  std::string source; // 本场已收到的公告原文，供模型解释和本地证据核对。
  for (const auto &message : announcements_) { // message是一批已去重的原公告。
    if (!source.empty()) source += '\n';
    source += message;
  }
  // 超限不截断公告制造语义；仍维持本地决策，日志明确显示不可解析。
  if (source.size() > 131072) { status_ = "announcement_history_too_large"; return; }
  // 开局understood_=false：即使公告为空，首帧也排一次请求；不用等随机事件才调用。
  if (busy_ || pending_ || ready_ || std::chrono::steady_clock::now() < retry_after_ ||
      invalid_replies_ >= 3 || (understood_ && source == understood_source_)) return;
  // tick给模型定位公告时间，不要求模型在该Tick内返回。每Tick动作由本地策略独立生成。
  Json context = {{"session_id", session_}, {"tick", p.at("tick")}, {"map", p.at("map")}, // 用户消息内容。
                  {"announcements", source}, {"known_events", known_events}};
  Json body = {{"messages", Json::array({{{"role", "system"}, {"content", event_system_prompt()}}, // Chat Completions请求。
                                          {{"role", "user"}, {"content", context.dump()}}})},
               {"model", config_.model}, {"max_tokens", config_.max_tokens}, {"stream", false}};
  if (!config_.thinking.empty()) body["thinking"] = {{"type", config_.thinking}};
  if (config_.json_output) body["response_format"] = {{"type", "json_object"}};
  const auto serialized = body.dump(); // 真正发出的JSON文本；发送前检查2MiB上限。
  if (serialized.size() > 2097152) { status_ = "request_too_large"; return; }
  pending_ = Job{session_, source, serialized, generation_.load()};
  status_ = "queued";
  wake_.notify_one();
}

std::optional<AiWorker::Result> AiWorker::poll() {
  // 只拿走现成结果，绝不在这里等待网络。
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  auto result = std::move(ready_); // 将结果所有权交给调用方，并清空槽位。
  ready_.reset();
  return result;
}

void AiWorker::acknowledge(const Result &r, bool understood, bool partial) {
  // r是请求结果；understood表示公告已完整理解；partial表示仍有unknown。
  // 退避按单调时钟计时，不会改变比赛Tick或事件到期时间。
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  if (r.generation != generation_.load() || r.session != session_) return;
  http_success_ = http_success_ || r.http_success;
  disabled_ = r.stop;
  status_ = r.status;
  if (r.mock_response) { status_ = "mock_gateway"; return; }
  if (understood && !partial) {
    understood_ = true; understood_source_ = r.source;
    failures_ = invalid_replies_ = 0;
    status_ = "events_validated";
  } else {
    if (r.http_success) {
      ++invalid_replies_;
      // HTTP已成功也可能没有最终答案；保留精确原因，不能全改成invalid_model_reply。
      status_ = partial ? "events_incomplete" : (r.reply.empty() ? r.status : "invalid_model_reply");
    }
    failures_ = std::min(failures_ + 1, 4U);
    retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1U << (failures_ - 1));
  }
}

std::string AiWorker::status() const {
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  return status_;
}
bool AiWorker::observed_http_success() const {
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  return http_success_;
}
Json AiWorker::diagnostics() const {
  // 返回快照后马上释放锁；日志只含次数/错误码，不含URL中的凭据或令牌。
  std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
  return {{"started", started_}, {"finished", finished_}, {"http_successes", successes_},
          {"last_http", last_http_}, {"last_curl", last_curl_}, {"known_mock", known_mock_},
          {"elapsed_ms", last_elapsed_ms_}, {"connect_ms", last_connect_ms_},
          {"first_byte_ms", last_first_byte_ms_}, {"last_result", last_result_}};
}

AiWorker::Result AiWorker::perform(const Job &job) {
  Result result; // 结果始终带上来源，任何失败也能回到正确场次判断。
  result.session = job.session; result.source = job.source; result.generation = job.generation;
  result.status = "network_error";
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup); // 自动释放本次curl句柄。
  if (!curl) return result;
  curl_slist *raw_headers = nullptr; // curl使用的原始请求头链表。
  raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
  raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + config_.token).c_str());
  raw_headers = curl_slist_append(raw_headers, "Expect:");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all); // 接管请求头释放。
  Download response; // 收HTTP正文的缓冲，存活到curl请求结束。
  Cancellation cancel{generation_, job.generation}; // 当前代数与此任务代数，用于跨场取消。
  curl_easy_setopt(curl.get(), CURLOPT_URL, config_.url.c_str());
  // 只走配置中指定的网关/代理，不继承终端HTTP_PROXY或NO_PROXY的隐式路径。
  curl_easy_setopt(curl.get(), CURLOPT_PROXY, config_.proxy.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "");
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, job.body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(job.body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, config_.timeout_ms);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, config_.connect_timeout_ms);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &Download::append);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &Cancellation::progress);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancel);
  const auto code = curl_easy_perform(curl.get()); // 阻塞只发生在后台线程；code是传输结果。
  result.curl_code = static_cast<int>(code);
  long status = 0; // HTTP状态码，与curl传输错误是两个不同维度。
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  result.http_status = status;
  double elapsed = 0, connected = 0, first_byte = 0; // curl以秒返回计时；只记录耗时，不记录响应内容。
  curl_easy_getinfo(curl.get(), CURLINFO_TOTAL_TIME, &elapsed);
  curl_easy_getinfo(curl.get(), CURLINFO_CONNECT_TIME, &connected);
  curl_easy_getinfo(curl.get(), CURLINFO_STARTTRANSFER_TIME, &first_byte);
  result.elapsed_ms = static_cast<long>(elapsed * 1000);
  result.connect_ms = static_cast<long>(connected * 1000);
  result.first_byte_ms = static_cast<long>(first_byte * 1000);
  if (code != CURLE_OK) {
    result.status = code == CURLE_OPERATION_TIMEDOUT ? "timeout" : "curl_" + std::to_string(result.curl_code);
    return result;
  }
  result.http_success = status >= 200 && status < 300;
  result.status = "http_" + std::to_string(status);
  if (status == 401 || status == 403 || status == 400 || status == 404) result.stop = true;
  try {
    const auto root = Json::parse(response.body); // 外层HTTP响应，不是内层事件JSON。
    if (result.http_success) {
      // 公开练习包明确标记的固定模拟响应。HTTP成功不等于LLM被调用。
      result.mock_response = root.value("model", std::string()) == "practice-mock";
      if (result.mock_response) { result.stop = true; result.status = "mock_gateway"; }
      else {
        const auto &choice = root.at("choices").at(0); // 只读第一条最终回答，不读取reasoning_content当答案。
        const auto finish = choice.value("finish_reason", std::string()); // length是截断；不能补括号冒充完整解释。
        const auto &message = choice.at("message"); // assistant消息外壳。
        const auto &content = message.at("content"); // 最终答案；可能为null或空字符串。
        if (finish == "length") result.status = "model_output_truncated";
        else if (!finish.empty() && finish != "stop") result.status = "model_finish_" + finish;
        else if (!content.is_string() || content.get_ref<const std::string &>().find_first_not_of(" \t\r\n") == std::string::npos) {
          const auto reasoning = message.find("reasoning_content"); // 只判断是否存在思考正文，不保存或打印它。
          result.status = reasoning != message.end() && reasoning->is_string() && !reasoning->get_ref<const std::string &>().empty()
                            ? "model_reasoning_only" : "model_empty_content";
        } else { result.reply = content.get<std::string>(); result.status = "model_reply_received"; }
      }
    }
    else if (status == 429 && root.at("error").value("code", std::string()) == "quota_exceeded") {
      result.stop = true; result.status = "quota_exceeded";
    }
  } catch (const Json::exception &) {
    if (result.http_success) result.status = "invalid_completion_response";
  }
  return result;
}

void AiWorker::run() {
  // job在锁内取走，HTTP在锁外执行；否则主线程poll/observe会一起卡住。
  for (;;) {
    Job job; // 当前这次循环独占的任务。
    {
      std::unique_lock<std::mutex> lock(mutex_); // 条件变量等待时可暂时释放的锁。
      wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
      if (stopping_) return;
      job = std::move(*pending_); pending_.reset(); busy_ = true; ++started_;
      status_ = "requesting"; // queued仅表示排队；后台已开始时明确改成requesting。
    }
    Result result; // 网络结果或捕获异常后合成的失败结果。
    try { result = perform(job); }
    catch (...) {
      result.session = job.session; result.source = job.source;
      result.generation = job.generation; result.status = "request_failed";
    }
    {
      std::lock_guard<std::mutex> lock(mutex_); // 本作用域持有共享状态锁，离开作用域自动释放。
      busy_ = false;
      if (job.generation == generation_.load() && job.session == session_) {
        ++finished_;
        successes_ += result.http_success;
        last_http_ = result.http_status; last_curl_ = result.curl_code;
        last_elapsed_ms_ = result.elapsed_ms; last_connect_ms_ = result.connect_ms;
        last_first_byte_ms_ = result.first_byte_ms; last_result_ = result.status;
        known_mock_ = known_mock_ || result.mock_response;
        ready_ = std::move(result);
      }
    }
  }
}
}  // namespace rmtc


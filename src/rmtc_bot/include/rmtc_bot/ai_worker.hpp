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

namespace rmtc { // 本工程的公共类型；与ROS、curl和标准库名字隔开。
// 一个HTTP客户端所需的固定设置，实际值全部来自match_config.hpp。
struct AiConfig {
  std::string url, token, model; // 完整网关地址、席位令牌、明确模型名。
  long timeout_ms = 0; // 整次HTTP请求的等待上限，毫秒；0表示尚未配置。
  int max_tokens = 0;  // 请求允许的输出token数；0表示尚未配置。
  std::string proxy;   // 到网关的网络代理；空串明确表示直连。
  long connect_timeout_ms = 0; // 连接阶段上限；必须明确填写且不超过整次请求上限。
  std::string thinking; // enabled/disabled，或空串表示不发送这个服务商扩展字段。
  bool json_output = false; // 是否明确发送response_format=json_object；生产值来自统一配置。
  int max_calls = 0; // 本场最多发起请求次数；0=不限。现场配额公布后在调整窗口内改参数。
};

// 主线程负责observe/poll，后台线程负责perform；等待网络不阻塞选动作。
class AiWorker {
public:
  // 传回主线程的一次请求结果。HTTP成功与事件理解成功分别记录。
  struct Result {
    std::string session, source, reply, status; // 场次、原公告证据、模型文本、请求状态。
    std::uint64_t generation = 0; // 本地换场代数，旧代结果必须丢弃。
    std::int64_t request_tick = -1; // 发起请求时的逻辑Tick；-1表示没有来源，不能用返回时刻倒推。
    long http_status = 0; // 服务返回的HTTP状态；没收到时为0。
    int curl_code = 0; // curl传输错误码；0表示传输完成，28是超时。
    long elapsed_ms = 0, connect_ms = 0, first_byte_ms = 0; // 整次、连接完成、首字节时刻；均相对请求开始。
    bool http_success = false, stop = false, mock_response = false; // 完整2xx、停止本场重试、明确模拟响应。
  };
  explicit AiWorker(AiConfig config); // 检查配置，配置完整时启动后台线程。
  ~AiWorker(); // 通知取消并join，防止线程访问已销毁对象。
  // 线程/锁的所有权不能复制，显式禁止复制本对象。
  AiWorker(const AiWorker &) = delete;
  AiWorker &operator=(const AiWorker &) = delete;
  void reset(const std::string &session); // 换场：清公告、重试、统计，并让旧请求失效。
  void observe(const nlohmann::json &perception, const nlohmann::json &known_events); // 收集公告，条件允许才排队请求。
  std::optional<Result> poll(); // 取走一个已完成结果；没结果就立即返回空。
  void acknowledge(const Result &result, bool understood, bool partial = false); // 主线程反馈JSON/事件校验是否通过。
  std::string status() const; // 返回线程安全的日志状态。
  bool enabled() const { return enabled_; } // 是否具备可用HTTP配置；不表示已有成功调用。
  bool observed_http_success() const; // 本场是否至少收到一次完整2xx，不代表正式AI资格。
  nlohmann::json diagnostics() const; // 返回本场调用次数和最近错误，不含密钥。

private:
  struct Job {
    std::string session, source, body; // 所属场次、原公告、已序列化的HTTP请求体。
    std::uint64_t generation; // 创建请求时的本地代数。
    std::int64_t request_tick; // 此任务建立时的感知Tick，与HTTP墙钟耗时分开。
  };
  Result perform(const Job &job); // 只在后台运行，一次完整POST，不在这里作策略决策。
  void run(); // 等待任务→释放锁→联网→加锁放回结果。
  AiConfig config_; // 启动后不再改变的请求配置。
  bool enabled_ = false, stopping_ = false, busy_ = false, disabled_ = false; // 配置可用、正在退出、请求在途、本场已停用。
  bool http_success_ = false, understood_ = false; // 本场收到过2xx、当前公告批次已完整理解。
  unsigned failures_ = 0, invalid_replies_ = 0; // 连续失败次数、当前公告批次的坏/不完整回复次数。
  unsigned started_ = 0, finished_ = 0, successes_ = 0; // 本场开始、完成、完整HTTP成功次数。
  long last_http_ = 0; // 最近一次HTTP状态码。
  int last_curl_ = 0; // 最近一次curl传输结果。
  bool known_mock_ = false; // 本场是否见过明确标记的practice-mock响应。
  long last_elapsed_ms_ = 0, last_connect_ms_ = 0, last_first_byte_ms_ = 0; // 最近请求耗时；首字节0表示尚无首字节计时。
  std::string last_result_; // 最近后台结果类型；acknowledge不覆盖它，便于区分传输与解析失败。
  std::string session_, understood_source_, status_ = "disabled"; // 当前场次、已理解的原文批次、日志状态。
  std::vector<std::string> announcements_; // 本场按收到顺序保存的公告文本批次。
  std::set<std::string> seen_; // 已收过的完整文本批次，防止重复加入。
  std::optional<Job> pending_; // 待后台线程取走的任务，最多一个。
  std::optional<Result> ready_; // 待主线程取走的结果，最多一个。
  std::chrono::steady_clock::time_point retry_after_{}; // 允许重试的单调时钟时刻，与比赛Tick无关。
  mutable std::mutex mutex_; // 保护共享状态；mutable允许只读日志函数也加锁。
  std::condition_variable wake_; // 有新任务或退出时唤醒后台线程。
  std::atomic<std::uint64_t> generation_{0}; // curl取消回调无需拿mutex就能检查是否已换场。
  std::thread thread_; // 唯一后台请求线程。
};
}  // namespace rmtc

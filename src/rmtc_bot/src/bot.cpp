#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/session.hpp"
#include "rmtc_bot/match_config.hpp"

using Json = nlohmann::json;
using Message = std_msgs::msg::String;
namespace cfg = rmtc::match_config;

class Bot final : public rclcpp::Node {
public:
  Bot() : Node("rmtc_bot") {
    const std::string team = cfg::team;
    const std::string name = cfg::team_name;
    if (team != "A" && team != "B" && team != "C" && team != "D") {
      throw std::runtime_error("team must be A, B, C or D");
    }
    if (name.empty() || name.size() > 96 ||
        name.find_first_of("\r\n") != std::string::npos) {
      throw std::runtime_error("team_name must contain 1-96 UTF-8 bytes and no newline");
    }
    const int stage = cfg::stage;
    if (stage != 1 && stage != 2) throw std::runtime_error("stage must be 1 or 2");
    rmtc::AiConfig ai{cfg::ai_url, cfg::ai_token, cfg::ai_model, cfg::ai_timeout_ms, cfg::ai_max_tokens};
    // 缺配置就停在赛前，不能悄悄关闭AI，等打完才发现成功次数为零。
    if (stage == 2 && (ai.url.empty() || ai.token.empty() || ai.model.empty()))
      throw std::runtime_error("Fill ai_url, ai_token and ai_model in match_config.hpp, then rebuild");
    if (stage == 2 && (ai.url.size() < 17 || ai.url.compare(ai.url.size() - 17, 17, "/chat/completions") != 0))
      throw std::runtime_error("ai_url must be the full /chat/completions endpoint");
    session_ = std::make_unique<rmtc::Session>(static_cast<int>(stage), std::move(ai));
    const auto prefix = "/team_" + team;
    const auto live = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile();
    const auto retained = rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    action_pub_ = create_publisher<Message>(prefix + "/action", live);
    registration_pub_ = create_publisher<Message>(prefix + "/registration", retained);
    perception_sub_ = create_subscription<Message>(
        prefix + "/perception", live,
        [this](Message::ConstSharedPtr msg) { on_perception(*msg); });
    registration_.data = Json{{"team", team}, {"name", name}}.dump();
    registration_pub_->publish(registration_);
    RCLCPP_INFO(get_logger(), "Ready: seat=%s name=%s", team.c_str(), name.c_str());
  }

private:
  void on_perception(const Message &msg) {
    try {
      const auto p = Json::parse(msg.data); // 当前完整公开感知。
      const auto action = session_->act(p); // 可选完整动作；为空时本回调不发布。
      if (action) {
        Message out; // 待发布的ROS消息外壳。
        out.data = action->dump();
        action_pub_->publish(out);
      }
      const auto receipt = p["self"].value("last_action", Json()).dump(); // 上Tick真实执行结果，供人工核对。
      const auto position = p["self"].value("position", Json()).dump(); // 裁判给出的当前绝对坐标，不按指令累加。
      const auto session = p.at("session_id").get<std::string>(); // 用它确认终端和网页在看同一场。
      const auto sent = action ? action->at("action").get<std::string>() : "none"; // none表示本回调未发布。
      const auto direction = action ? action->value("direction", std::string("-")) : "-"; // 计划方向，区别于回执的实际方向。
      const auto navigation=session_->navigation_diagnostics().dump(); // 当前无进展时长、脱困目标和暂缓数。
      const auto ai = session_->ai_diagnostics().dump(); // 本场HTTP统计和最近传输状态。
      const auto mode = rmtc::turn_mode_name(rmtc::turn_mode(p, cfg::stage)); // 本帧动作权限状态，与状态机图一致。
      RCLCPP_INFO(get_logger(), "session=%s tick=%lld pos=%s mode=%s sent=%s direction=%s reason=%s ai=%s http_success=%d ai_calls=%s navigation=%s last_action=%s",
                  session.c_str(),
                  static_cast<long long>(rmtc::frame_tick(p)),
                  position.c_str(), mode, sent.c_str(), direction.c_str(), session_->reason().c_str(),
                  session_->ai_status().c_str(), session_->ai_http_success(), ai.c_str(), navigation.c_str(), receipt.c_str());
    } catch (const std::exception &e) { // e只输出错误原因，不输出感知全文、令牌或请求头。
      RCLCPP_WARN(get_logger(), "Skipping invalid perception: %s", e.what());
    }
  }

  std::unique_ptr<rmtc::Session> session_;
  Message registration_;
  rclcpp::Publisher<Message>::SharedPtr action_pub_;
  rclcpp::Publisher<Message>::SharedPtr registration_pub_;
  rclcpp::Subscription<Message>::SharedPtr perception_sub_;
};

int main(int argc, char **argv) {
  // DDS在init时读这些变量；这里用固定配置设置，启动终端不用手工export。
  if (::setenv("ROS_DOMAIN_ID", cfg::ros_domain_id, 1) != 0 ||
      ::setenv("ROS_LOCALHOST_ONLY", cfg::ros_localhost_only, 1) != 0 ||
      ::setenv("RMW_IMPLEMENTATION", cfg::rmw_implementation, 1) != 0) {
    std::cerr << "Cannot apply ROS settings from match_config.hpp\n";
    return 1;
  }
  rclcpp::init(argc, argv);
  int status = 0;
  try {
    rclcpp::spin(std::make_shared<Bot>());
  } catch (const std::exception &e) {
    RCLCPP_ERROR(rclcpp::get_logger("rmtc_bot"), "%s", e.what());
    status = 1;
  }
  rclcpp::shutdown();
  return status;
}


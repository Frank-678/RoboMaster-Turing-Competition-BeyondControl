#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/session.hpp"

using Json = nlohmann::json;
using Message = std_msgs::msg::String;

class Bot final : public rclcpp::Node {
public:
  Bot() : Node("rmtc_bot") {
    const auto team = declare_parameter<std::string>("team", "A");
    const auto name = declare_parameter<std::string>("team_name", "Frank");
    if (team != "A" && team != "B" && team != "C" && team != "D") {
      throw std::runtime_error("team must be A, B, C or D");
    }
    if (name.empty() || name.size() > 96 ||
        name.find_first_of("\r\n") != std::string::npos) {
      throw std::runtime_error("team_name must contain 1-96 UTF-8 bytes and no newline");
    }
    const auto stage = declare_parameter<std::int64_t>("stage", 2);
    if (stage != 1 && stage != 2) throw std::runtime_error("stage must be 1 or 2");
    const auto env = [](const char *key) {
      const char *value = std::getenv(key);
      return value ? std::string(value) : std::string();
    };
    rmtc::AiConfig ai;
    ai.url = env("RMTC_AI_URL"); ai.token = env("RMTC_TEAM_TOKEN"); ai.model = env("RMTC_AI_MODEL");
    if (!env("RMTC_AI_MAX_TOKENS").empty()) ai.max_tokens = std::stoi(env("RMTC_AI_MAX_TOKENS"));
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
    if (stage == 2 && (env("RMTC_AI_URL").empty() || env("RMTC_TEAM_TOKEN").empty()))
      RCLCPP_WARN(get_logger(), "AI disabled: set RMTC_AI_URL and RMTC_TEAM_TOKEN; formal AI qualification is unverified");
  }

private:
  void on_perception(const Message &msg) {
    try {
      const auto p = Json::parse(msg.data);
      const auto action = session_->act(p);
      if (!action) return;
      Message out;
      out.data = action->dump();
      action_pub_->publish(out);
      const auto receipt = p["self"].value("last_action", Json()).dump();
      RCLCPP_INFO(get_logger(), "tick=%lld sent=%s reason=%s ai=%s http_success=%d last_action=%s",
                  static_cast<long long>(rmtc::frame_tick(p)),
                  action->at("action").get<std::string>().c_str(), session_->reason().c_str(),
                  session_->ai_status().c_str(), session_->ai_http_success(), receipt.c_str());
    } catch (const std::exception &e) {
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

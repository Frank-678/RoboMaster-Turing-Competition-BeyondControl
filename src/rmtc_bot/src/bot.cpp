#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include "rmtc_bot/protocol.hpp"
#include "rmtc_bot/strategy.hpp"

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
      const auto p = Json::parse(msg.data);
      const auto tick = rmtc::frame_tick(p);
      const auto session = p["session_id"].get<std::string>();
      if (session != session_) {
        strategy_.reset();
        session_ = session;
        latest_tick_ = -1;
        cached_action_ = Json();
      }
      if (tick < latest_tick_) {
        return;
      }
      // Repeated perception after pause: resend, without advancing the strategy.
      if (tick > latest_tick_) {
        Json proposed = {{"action", "wait"}};
        try {
          proposed = strategy_.decide(p);
        } catch (const std::exception &e) {
          RCLCPP_WARN(get_logger(), "Strategy failed; sending wait: %s", e.what());
        }
        cached_action_ = rmtc::action_for(p, proposed);
        latest_tick_ = tick;
      }
      Message out;
      out.data = cached_action_.dump();
      action_pub_->publish(out);
      const auto receipt = p["self"].value("last_action", Json()).dump();
      RCLCPP_INFO(get_logger(), "tick=%lld sent=%s last_action=%s",
                  static_cast<long long>(tick),
                  cached_action_["action"].get<std::string>().c_str(), receipt.c_str());
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "Skipping invalid perception: %s", e.what());
    }
  }

  Strategy strategy_;
  std::string session_;
  std::int64_t latest_tick_ = -1;
  Json cached_action_;
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

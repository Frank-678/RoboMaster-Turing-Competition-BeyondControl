// 冻结版本的窄接口：只链接那个提交的策略/战术/事件代码，不链接本轮策略或裁判。
// 一个进程服务一辆车的一场比赛；对象跨Tick保留，换场新建进程。没有HTTP、ROS或全图入口。
#include "rmtc_bot/strategy.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

using Json=nlohmann::json;

int main() {
  try {
    std::string line;
    if(!std::getline(std::cin,line)) throw std::runtime_error("worker needs init");
    const auto init=Json::parse(line);
    const int stage=init.at("stage");
    if(init.at("op")!="init" || (stage!=1 && stage!=2)) throw std::runtime_error("worker init schema");
    Strategy strategy(stage);
    const auto parameters=rmtc::StrategyParameters::competition();
    Json values=Json::object();
    // 由构建脚本从该版本StrategyParameters的成员声明生成；只导出策略参数，不导出令牌。
#include "version_parameters.inc"
    std::cout<<Json{{"ready",true},{"parameters",values}}.dump()<<std::endl;
    while(std::getline(std::cin,line)) {
      const auto request=Json::parse(line);
      if(request.at("op")=="quit") break;
      if(request.at("op")!="act") throw std::runtime_error("worker expects act");
      const auto &input=request.at("input"),&p=input.at("perception");
      std::vector<rmtc::Event> known;
      for(const auto &entry:input.at("events")) {
        const auto &event=entry.at("event");
        auto decoded=rmtc::decode_events(Json{{"session_id",p.at("session_id")},
          {"events",Json::array({event})},{"unknown",Json::array()}}.dump(),
          p.at("session_id"),event.at("source"),p.at("map").at("width"),p.at("map").at("height"));
        decoded.at(0).key=entry.at("key");known.push_back(decoded.at(0));
      }
      strategy.merge_events(known);
      if(input.at("qualified").get<bool>()) strategy.note_model_reply();
      const auto begin=std::chrono::steady_clock::now();
      const auto action=strategy.decide(p);
      const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
      std::cout<<Json{{"action",action},{"reason",strategy.reason()},{"decide_ms",elapsed},
                       {"tick",p.at("tick")},{"session_id",p.at("session_id")}}.dump()<<std::endl;
    }
    return 0;
  } catch(const std::exception &error) {std::cerr<<"version worker: "<<error.what()<<'\n';return 2;}
}

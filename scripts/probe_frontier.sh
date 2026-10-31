#!/bin/bash
set -u
cd /home/frank/cst
H=/tmp/head_src
rm -rf "$H"; mkdir -p "$H/src/rmtc_bot/include/rmtc_bot" "$H/src/rmtc_bot/src"
for f in src/rmtc_bot/src/strategy.cpp src/rmtc_bot/src/tactics.cpp src/rmtc_bot/src/events.cpp \
         src/rmtc_bot/src/ai_worker.cpp src/rmtc_bot/src/session.cpp \
         src/rmtc_bot/include/rmtc_bot/strategy_parameters.hpp src/rmtc_bot/include/rmtc_bot/match_config.hpp; do
  git show "HEAD:$f" > "$H/$f"
done
cat > /tmp/probe_main.cpp <<'EOF'
#include "rmtc_bot/session.hpp"
#include <iostream>
using Json = nlohmann::json;
// 与 tests/strategy_test.cpp 完全一致的感知夹具；9x9，position是{col,row}。
Json frame(int x, int y, int width, int height, int radius) {
  return {{"tick", 0}, {"session_id", "test-match"}, {"vision_radius", radius},
          {"self", {{"alive", true}, {"position", {x, y}}, {"protected", false}}},
          {"map", {{"width", width}, {"height", height}}},
          {"visible_entities", Json::array()}};
}
int main() {
  auto p = frame(4, 1, 9, 9, 1);
  rmtc::Session s1(1), s2(2);
  auto a1 = s1.act(p); auto a2 = s2.act(p);
  std::cout << "stage1: action=" << (*a1)["action"] << " direction=" << (*a1)["direction"] << " reason=" << s1.reason() << "\n";
  std::cout << "stage2: action=" << (*a2)["action"] << " direction=" << (*a2)["direction"] << " reason=" << s2.reason() << "\n";
  return 0;
}
EOF
T=$(mktemp -d)
echo "=== WORKING TREE ==="
g++ -std=c++17 -O1 -pthread -I src/rmtc_bot/include src/rmtc_bot/src/strategy.cpp src/rmtc_bot/src/tactics.cpp src/rmtc_bot/src/events.cpp src/rmtc_bot/src/ai_worker.cpp src/rmtc_bot/src/session.cpp /tmp/probe_main.cpp -lcurl -o "$T/probe_work" || echo WORK_BUILD_FAIL
"$T/probe_work"
echo "=== HEAD ==="
g++ -std=c++17 -O1 -pthread -I "$H/src/rmtc_bot/include" -I src/rmtc_bot/include "$H"/src/rmtc_bot/src/strategy.cpp "$H"/src/rmtc_bot/src/tactics.cpp "$H"/src/rmtc_bot/src/events.cpp "$H"/src/rmtc_bot/src/ai_worker.cpp "$H"/src/rmtc_bot/src/session.cpp /tmp/probe_main.cpp -lcurl -o "$T/probe_head" || echo HEAD_BUILD_FAIL
"$T/probe_head"
rm -rf "$T" "$H" /tmp/probe_main.cpp

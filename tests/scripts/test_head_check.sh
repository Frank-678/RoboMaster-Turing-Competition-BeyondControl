#!/bin/bash
set -u
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
H=/tmp/head_src
rm -rf "$H"; mkdir -p "$H/src/rmtc_bot/include/rmtc_bot" "$H/src/rmtc_bot/src" "$H/tests"
for f in src/rmtc_bot/src/strategy.cpp src/rmtc_bot/src/tactics.cpp src/rmtc_bot/src/events.cpp \
         src/rmtc_bot/src/ai_worker.cpp src/rmtc_bot/src/session.cpp \
         src/rmtc_bot/include/rmtc_bot/strategy_parameters.hpp src/rmtc_bot/include/rmtc_bot/match_config.hpp \
         tests/full_strategy_test.cpp tests/strategy_test.cpp; do
  git show "HEAD:$f" > "$H/$f"
done
T=$(mktemp -d)
# HEAD 的策略头/测试优先；其余头文件（strategy.hpp 等）来自当前工作树。
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread -I "$H/src/rmtc_bot/include" -I src/rmtc_bot/include \
  "$H"/src/rmtc_bot/src/strategy.cpp "$H"/src/rmtc_bot/src/tactics.cpp "$H"/src/rmtc_bot/src/events.cpp \
  "$H"/src/rmtc_bot/src/ai_worker.cpp "$H"/src/rmtc_bot/src/session.cpp \
  "$H"/tests/strategy_test.cpp "$H"/tests/full_strategy_test.cpp -lcurl -o "$T/strategy_test_head" || { echo "HEAD_BUILD_FAIL"; rm -rf "$T" "$H"; exit 9; }
echo "HEAD_BUILD_OK"
python3 tests/mock_gateway.py "$T/strategy_test_head"
echo "HEAD_GATEWAY_EXIT=$?"
rm -rf "$T" "$H"

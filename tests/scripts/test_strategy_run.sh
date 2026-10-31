#!/bin/bash
set -u
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
T=$(mktemp -d)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread -I src/rmtc_bot/include \
  src/rmtc_bot/src/strategy.cpp src/rmtc_bot/src/tactics.cpp src/rmtc_bot/src/events.cpp \
  src/rmtc_bot/src/ai_worker.cpp src/rmtc_bot/src/session.cpp \
  tests/strategy_test.cpp tests/full_strategy_test.cpp -lcurl -o "$T/strategy_test"
echo "BUILD_OK"
python3 tests/mock_gateway.py "$T/strategy_test"
echo "GATEWAY_EXIT=$?"
rm -rf "$T"

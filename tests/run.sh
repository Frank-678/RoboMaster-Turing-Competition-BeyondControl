#!/usr/bin/env bash
set -euo pipefail
rmtc_test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" # 测试工程根目录。
rmtc_test_tmp="$(mktemp -d)" # 临时可执行文件目录，退出时自动删除。
trap 'rm -rf -- "$rmtc_test_tmp"' EXIT
python3 "$rmtc_test_root/tests/startup_test.py"
rmtc_test_flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread # 编译参数数组，保证带空格的路径不会拆错。
  -I "$rmtc_test_root/src/rmtc_bot/include")
if [[ -n "${RMTC_JSON_INCLUDE:-}" ]]; then
  rmtc_test_flags+=(-I "$RMTC_JSON_INCLUDE")
fi
if [[ "${RMTC_SANITIZE:-0}" == 1 ]]; then
  rmtc_test_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
g++ "${rmtc_test_flags[@]}" \
  "$rmtc_test_root"/src/rmtc_bot/src/{strategy,events,ai_worker,session}.cpp \
  "$rmtc_test_root"/tests/{strategy_test,full_strategy_test}.cpp \
  -lcurl -o "$rmtc_test_tmp/strategy_test"
python3 "$rmtc_test_root/tests/mock_gateway.py" "$rmtc_test_tmp/strategy_test"
# 可选参数仅指定待只读加载的原版practice.py；完整仓库中不传参数即可。
rmtc_practice_source="${1:-$rmtc_test_root/practice/practice.py}"
if [[ -f "$rmtc_practice_source" ]]; then
  PYTHONDONTWRITEBYTECODE=1 python3 "$rmtc_test_root/tests/ai_check_test.py"
  PYTHONDONTWRITEBYTECODE=1 python3 "$rmtc_test_root/tests/practice_gateway.py" "$rmtc_test_tmp/strategy_test" "$rmtc_practice_source"
else
  echo '原版practice.py未在本地，跳过原版HTTP转发测试；核心与固定HTTP测试已完成。'
fi

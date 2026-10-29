#!/usr/bin/env bash
set -eo pipefail
# 场景文件和结果都在测试目录；不读比赛网关，不启动ROS。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
exec python3 tests/run_scenarios.py "$@"

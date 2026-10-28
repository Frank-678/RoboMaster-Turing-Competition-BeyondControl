#!/usr/bin/env bash
set -eo pipefail
# 固定到工程根目录，避免在src子目录构建出另一个工作空间。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source /opt/ros/humble/setup.bash
# 只保存配置摘要，不保存密钥正文。run/practice用它拒绝“改头文件但没重新编译”的旧程序。
rmtc_config_digest="$(sha256sum src/rmtc_bot/include/rmtc_bot/match_config.hpp)"
# 只构建选手包；Debug便于定位，编译数据库供编辑器跳转使用。
colcon build --symlink-install --base-paths src --packages-select rmtc_bot \
  --cmake-args -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sfn build/rmtc_bot/compile_commands.json compile_commands.json
printf '%s\n' "$rmtc_config_digest" > build/rmtc_bot/match_config.sha256
# 新诊断入口必须配套新启动器；旧启动器会忽略--check-ai，不能让它误启动裁判。
sha256sum src/rmtc_bot/src/practice_launcher.cpp scripts/check_ai.py check_ai.sh \
  install/rmtc_bot/lib/rmtc_bot/practice_launcher > build/rmtc_bot/ai_check.sha256
echo "构建完成：$PWD/install/rmtc_bot/lib/rmtc_bot/bot"

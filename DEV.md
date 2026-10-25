# 选手工程

工作空间：`~/cst`。语言：C++17。ROS：Humble。包名：`rmtc_bot`。可执行文件：`bot`。

## 首次使用

在 Ubuntu / WSL 中执行：

```bash
sudo apt update
sudo apt install -y build-essential cmake python3-colcon-common-extensions nlohmann-json3-dev ros-humble-rclcpp ros-humble-std-msgs ros-humble-rmw-fastrtps-cpp
cd ~/cst
bash build.sh
bash run.sh A 'Frank'
```

保持练习裁判在另一个终端运行；先退出此前占用 A 席位的参考车。打开 http://127.0.0.1:8080 并开始比赛，A 席位不要同时设为陪练。

## 写代码

| 文件 | 用途 |
| --- | --- |
| `src/rmtc_bot/src/strategy.cpp` | 在 `Strategy::decide()` 中编写策略 |
| `src/rmtc_bot/include/rmtc_bot/strategy.hpp` | 声明策略状态与辅助函数 |
| `src/rmtc_bot/src/bot.cpp` | ROS 通信、队名注册、场次与 Tick 管理 |
| `src/rmtc_bot/include/rmtc_bot/protocol.hpp` | 输入检查、动作字段组装及基础校验 |
| `src/rmtc_bot/CMakeLists.txt` | 增加新源文件或依赖 |
| `src/rmtc_bot/package.xml` | 依赖与包信息；维护者邮箱是待替换占位值 |

初始策略仅返回 `wait`。返回值示例：

```cpp
return {{"action", "move"}, {"direction", "right"}};
```

方向：`up`、`down`、`left`、`right`。动作：`move`、`attack`、`defend`、`wait`。
通信层自动补入当前 `tick` 和 `session_id`；同一 Tick 重发时复用缓存动作，新场次调用 `Strategy::reset()`。
程序检查动作与方向格式，阵亡时改为等待，已知保护期内攻击改为等待。
阶段限制、碰撞、争格、时限与其他规则仍须由策略处理，以裁判回执为准。

原样接收公开感知 JSON。`self.position` 是绝对坐标数组；可见实体的 `position` 是九行相对位置字符串。
当前代码仅订阅自己的 `/team_X/perception`；任务和近期事件文本也包含在感知中。
尚未接入 LLM，没有事件理解或采集、寻路、对抗策略。不要在同步 `decide()` 中执行耗时网络请求。
正式比赛前须按当场通知设置 ROS 网络参数、席位，另行实现并验证真实 AI 网关调用。

## 修改后运行

先用 Ctrl+C 停止当前机器人，再执行：

```bash
cd ~/cst
bash build.sh
bash run.sh A 'Frank'
```

也可以在新终端直接执行：

```bash
cd ~/cst
source /opt/ros/humble/setup.bash
source install/setup.bash
ros2 pkg prefix rmtc_bot
```

预期包路径：`/home/frank/cst/install/rmtc_bot`。
日志中 `sent=wait` 表示已发布动作；后续 `last_action` 回执才可用于确认裁判接收和执行。

## 验证范围

初始化脚本经过临时目录安装、重复运行保护、已有 README 与 practice 文件保留检查。
协议与策略部分使用 C++17 和 nlohmann/json 3.10.5 编译运行检查。
生成环境没有安装 ROS 2，未在此处执行 ROS 编译与实际 DDS 联调；请在你的 WSL 中运行上述命令完成验证。

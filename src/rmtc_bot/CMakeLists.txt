cmake_minimum_required(VERSION 3.16)
project(rmtc_bot)

# ament/ROS负责节点构建；JSON、curl、Threads分别用于协议、HTTP和后台线程。
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# 独立核心库不依赖ROS，可直接在离线测试中复用同一套逻辑。
add_library(rmtc_strategy STATIC src/strategy.cpp src/events.cpp src/ai_worker.cpp src/session.cpp)
target_compile_features(rmtc_strategy PUBLIC cxx_std_17)
target_include_directories(rmtc_strategy PUBLIC include)
target_link_libraries(rmtc_strategy PUBLIC nlohmann_json::nlohmann_json CURL::libcurl Threads::Threads)
# bot是ROS收发入口；practice_launcher只是给原版练习程序传入固定配置。
add_executable(bot src/bot.cpp)
ament_target_dependencies(bot rclcpp std_msgs)
target_link_libraries(bot rmtc_strategy)
add_executable(practice_launcher src/practice_launcher.cpp)
target_compile_features(practice_launcher PRIVATE cxx_std_17)
target_include_directories(practice_launcher PRIVATE include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # CMAKE_CXX_COMPILER_ID是当前编译器名；启用常见警告帮助发现类型/时序错误。
  target_compile_options(bot PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(rmtc_strategy PRIVATE -Wall -Wextra -Wpedantic)
endif()
include(CTest)
if(BUILD_TESTING)
  # BUILD_TESTING控制CTest；这里注册核心回归，HTTP夹具由tests/run.sh另外启动。
  enable_testing()
  add_executable(strategy_test ../../tests/strategy_test.cpp ../../tests/full_strategy_test.cpp)
  target_link_libraries(strategy_test rmtc_strategy)
  add_test(NAME strategy_regression COMMAND strategy_test)
endif()
install(TARGETS bot practice_launcher DESTINATION lib/${PROJECT_NAME})
# PROJECT_NAME为rmtc_bot；安装后ros2 run据此找到两个可执行文件。

ament_package()

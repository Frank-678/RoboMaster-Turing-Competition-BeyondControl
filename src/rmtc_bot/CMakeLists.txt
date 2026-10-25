cmake_minimum_required(VERSION 3.8)
project(rmtc_bot)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(bot src/bot.cpp src/strategy.cpp)
target_compile_features(bot PRIVATE cxx_std_17)
target_include_directories(bot PRIVATE include)
ament_target_dependencies(bot rclcpp std_msgs)
target_link_libraries(bot nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bot PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS bot DESTINATION lib/${PROJECT_NAME})

ament_package()

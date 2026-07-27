# 通用机器人业务编排层：组合 Master、标准 PDO 与内部命令桥接，不依赖具体从站。
add_library(robot_ethercat_application STATIC
    robot_ethercat_orchestrator.cpp
    robot_pdo_bridge.cpp
)
add_library(robot::ethercat_application ALIAS robot_ethercat_application)

target_include_directories(robot_ethercat_application
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/.."
)

target_link_libraries(robot_ethercat_application
    PUBLIC
    igh::core
    PRIVATE
    igh::device_cia402_standard
    robot_interface::robot_interface
    cia402::cia402
)

target_compile_options(robot_ethercat_application PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

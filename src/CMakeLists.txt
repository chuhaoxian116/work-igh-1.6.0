# 各模块分别声明自身库目标和依赖，当前文件只负责组合。
add_subdirectory(master)
add_subdirectory(device)
add_subdirectory(orchestrator)

find_package(Threads REQUIRED)

# 最终示例程序：选择具体从站，并启动唯一 EtherCAT 周期线程。
add_executable(robot_ethercat_app
    main.cpp
)

target_link_libraries(robot_ethercat_app PRIVATE
    robot::ethercat_application
    igh::device_gsd620
    Threads::Threads
    rt
)

set_target_properties(robot_ethercat_app PROPERTIES
    BUILD_RPATH
        "${ETHERCAT_LIBRARY_DIR};${CIA402_LIBRARY_DIR}"
)

target_compile_options(robot_ethercat_app PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

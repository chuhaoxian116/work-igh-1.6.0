# IgH 通用核心：只包含主站生命周期、Domain 周期收发和从站抽象接口。
add_library(igh_core STATIC
    master/igh_master.cpp
)
add_library(igh::core ALIAS igh_core)

target_include_directories(igh_core
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ETHERCAT_INCLUDE_DIR}"
)

target_link_libraries(igh_core
    PUBLIC
    "${ETHERCAT_LIBRARY}"
)

target_compile_options(igh_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# GSD620 具体从站适配：只链接 IgH 通用核心。
add_library(igh_device_gsd620 STATIC
    device/gsd620_device.cpp
)
add_library(igh::device_gsd620 ALIAS igh_device_gsd620)

target_include_directories(igh_device_gsd620
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(igh_device_gsd620
    PUBLIC
    igh::core
)

target_compile_options(igh_device_gsd620 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 当前机器人业务编排层：组合 Master 与具体从站，不属于 IgH 通用核心。
add_library(robot_ethercat_application STATIC
    orchestrator/robot_ethercat_orchestrator.cpp
)
add_library(robot::ethercat_application ALIAS robot_ethercat_application)

target_include_directories(robot_ethercat_application
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(robot_ethercat_application
    PUBLIC
    igh::core
    igh::device_gsd620
)

target_compile_options(robot_ethercat_application PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

find_package(Threads REQUIRED)

add_executable(robot_ethercat_app
    main.cpp
)

target_link_libraries(robot_ethercat_app PRIVATE
    robot::ethercat_application
    Threads::Threads
    rt
)

set_target_properties(robot_ethercat_app PROPERTIES
    BUILD_RPATH "${ETHERCAT_LIBRARY_DIR}"
)

target_compile_options(robot_ethercat_app PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

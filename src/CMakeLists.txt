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

# 标准 CiA402 PDO 设备适配：只负责 EtherCAT PDO/DC，不依赖状态机库。
add_library(igh_device_cia402_standard STATIC
    device/cia402_standard_pdo_device.cpp
)
add_library(igh::device_cia402_standard ALIAS igh_device_cia402_standard)

target_include_directories(igh_device_cia402_standard
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(igh_device_cia402_standard
    PUBLIC
    igh::core
)

target_compile_options(igh_device_cia402_standard PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# GSD620 具体从站适配：复用标准 CiA402 PDO 设备库。
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
    igh::device_cia402_standard
)

target_compile_options(igh_device_gsd620 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 通用机器人业务编排层：仅依赖 Master 和设备定义，不依赖具体从站。
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
    igh::device_gsd620
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

# 标准 CiA402 PDO 设备适配：只负责 EtherCAT PDO/DC，不依赖状态机库。
add_library(igh_device_cia402_standard STATIC
    cia402_standard_pdo_device.cpp
)
add_library(igh::device_cia402_standard ALIAS igh_device_cia402_standard)

target_include_directories(igh_device_cia402_standard
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/.."
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

# 具体从站适配各自在独立目录中声明构建目标。
add_subdirectory(gsd620)

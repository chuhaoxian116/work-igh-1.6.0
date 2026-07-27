# GSD620 具体从站适配：只保存设备身份、DC 参数和厂商专有扩展。
add_library(igh_device_gsd620 STATIC
    gsd620_device.cpp
)
add_library(igh::device_gsd620 ALIAS igh_device_gsd620)

target_include_directories(igh_device_gsd620
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../.."
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

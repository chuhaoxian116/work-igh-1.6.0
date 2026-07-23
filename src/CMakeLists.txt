add_library(igh_core STATIC
    device/gsd620_device.cpp
    master/igh_master.cpp
)

target_include_directories(igh_core
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ETHERCAT_INCLUDE_DIR}"
)

target_link_libraries(igh_core
    PUBLIC
    "${ETHERCAT_LIBRARY}"
    cia402::cia402
)

target_compile_options(igh_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

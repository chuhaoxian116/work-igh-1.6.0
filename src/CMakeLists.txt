add_library(igh_core STATIC
    device/gsd620_device.cpp
    master/igh_master.cpp
    orchestrator/robot_ethercat_orchestrator.cpp
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

find_package(Threads REQUIRED)

add_executable(robot_ethercat_app
    main.cpp
)

target_link_libraries(robot_ethercat_app PRIVATE
    igh_core
    Threads::Threads
    rt
)

set_target_properties(robot_ethercat_app PROPERTIES
    BUILD_RPATH "${ETHERCAT_LIBRARY_DIR};${CIA402_LIBRARY_DIR}"
)

target_compile_options(robot_ethercat_app PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

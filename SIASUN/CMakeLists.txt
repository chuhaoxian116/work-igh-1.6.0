add_library(siasun_core STATIC
    src/axis_config.cpp
    src/ethercat_app.cpp
    src/sdo_parameter.cpp
    ../thirdparty/tinyxml2/tinyxml2.cpp
)

target_include_directories(siasun_core
    PUBLIC
    include
    "${ETHERCAT_INCLUDE_DIR}"
    PRIVATE
    ../thirdparty/tinyxml2
)

target_link_libraries(siasun_core PUBLIC
    "${ETHERCAT_LIBRARY}"
    pthread
    m
    rt
)

target_compile_options(siasun_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(igh_master_sinsun
    src/main.cpp
)

set_target_properties(igh_master_sinsun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(igh_master_sinsun PRIVATE
    siasun_core
)

target_link_options(igh_master_sinsun PRIVATE
    "-Wl,-rpath,${ETHERCAT_LIBRARY_DIR}"
)

target_compile_options(igh_master_sinsun PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

option(SIASUN_BUILD_TESTS "Build SIASUN communication test demos" ON)

if(SIASUN_BUILD_TESTS)
    add_subdirectory(test)
endif()

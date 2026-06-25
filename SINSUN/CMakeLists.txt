add_executable(igh_master_sinsun
    src/main.cpp
    src/ethercat_app.cpp
)

set_target_properties(igh_master_sinsun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(igh_master_sinsun PRIVATE
    include
    "${ETHERCAT_INCLUDE_DIR}"
)

target_link_libraries(igh_master_sinsun PRIVATE
    "${ETHERCAT_LIBRARY}"
    pthread
    m
    rt
)

target_link_options(igh_master_sinsun PRIVATE
    "-Wl,-rpath,${ETHERCAT_LIBRARY_DIR}"
)

target_compile_options(igh_master_sinsun PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

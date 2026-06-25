add_executable(igh_master_gsd620
    src/main.c
    src/ethercat_master.c
    src/drive_pdo.c
)

set_target_properties(igh_master_gsd620 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(igh_master_gsd620 PRIVATE
    include
    "${ETHERCAT_INCLUDE_DIR}"
)

target_link_libraries(igh_master_gsd620 PRIVATE
    "${ETHERCAT_LIBRARY}"
    pthread
    m
    rt
)

target_link_options(igh_master_gsd620 PRIVATE
    "-Wl,-rpath,${ETHERCAT_LIBRARY_DIR}"
)

target_compile_options(igh_master_gsd620 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

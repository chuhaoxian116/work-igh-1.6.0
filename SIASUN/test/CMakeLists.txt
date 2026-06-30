add_executable(siasun_realtime_communication_test
    realtime_communication_test.cpp
)

target_link_libraries(siasun_realtime_communication_test PRIVATE
    siasun_core
)

target_link_options(siasun_realtime_communication_test PRIVATE
    "-Wl,-rpath,${ETHERCAT_LIBRARY_DIR}"
)

target_compile_options(siasun_realtime_communication_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

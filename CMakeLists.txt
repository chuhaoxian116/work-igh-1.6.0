cmake_minimum_required(VERSION 3.16)

project(igh_master
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ETHERCAT_PREFIX
    "${CMAKE_CURRENT_LIST_DIR}/../install/ethercat"
    CACHE PATH "IgH EtherCAT install prefix")

add_library(EtherCAT::EtherCAT SHARED IMPORTED)
set_target_properties(EtherCAT::EtherCAT PROPERTIES
    IMPORTED_LOCATION "${ETHERCAT_PREFIX}/lib/libethercat.so"
    INTERFACE_INCLUDE_DIRECTORIES "${ETHERCAT_PREFIX}/include"
)

add_executable(igh_master
    src/main.c
    src/ethercat_master.c
    src/drive_pdo.c
)

target_include_directories(igh_master PRIVATE
    include
)

target_link_libraries(igh_master PRIVATE
    EtherCAT::EtherCAT
    pthread
    m
    rt
)

target_link_options(igh_master PRIVATE
    "-Wl,-rpath,${ETHERCAT_PREFIX}/lib"
)

target_compile_options(igh_master PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

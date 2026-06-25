cmake_minimum_required(VERSION 3.16)

project(igh_master
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ETHERCAT_PREFIX
    "${CMAKE_CURRENT_LIST_DIR}/thirdparty/ethercat"
    CACHE PATH "IgH EtherCAT install prefix")

set(ETHERCAT_INCLUDE_DIR
    "${ETHERCAT_PREFIX}/include"
    CACHE PATH "IgH EtherCAT include directory")
set(ETHERCAT_LIBRARY
    "${ETHERCAT_PREFIX}/lib/libethercat.so"
    CACHE FILEPATH "IgH EtherCAT shared library")
set(ETHERCAT_LIBRARY_DIR
    "${ETHERCAT_PREFIX}/lib"
    CACHE PATH "IgH EtherCAT library directory")

add_subdirectory(GSD620)
add_subdirectory(SINSUN)

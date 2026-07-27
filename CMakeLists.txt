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
    "${CMAKE_CURRENT_LIST_DIR}/third_party/ethercat"
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

set(CIA402_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/cia402")
set(CIA402_INCLUDE_DIR "${CIA402_SDK_DIR}/include")
set(CIA402_LIBRARY_DIR "${CIA402_SDK_DIR}/lib")
set(CIA402_LIBRARY "${CIA402_LIBRARY_DIR}/libcia402.so")

set(ROBOT_INTERFACE_SDK_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/robot_interface")
set(ROBOT_INTERFACE_INCLUDE_DIR "${ROBOT_INTERFACE_SDK_DIR}/include")
set(ROBOT_INTERFACE_PUBLIC_HEADER
    "${ROBOT_INTERFACE_INCLUDE_DIR}/robot_data.h")

if(NOT EXISTS "${CIA402_INCLUDE_DIR}/cia402/cia402.h" OR
   NOT EXISTS "${CIA402_LIBRARY}" OR
   NOT EXISTS "${ROBOT_INTERFACE_PUBLIC_HEADER}")
    message(FATAL_ERROR
        "SDK submodules are not initialized. Run: "
        "git submodule update --init --recursive")
endif()

add_library(robot_interface::robot_interface INTERFACE IMPORTED GLOBAL)
set_target_properties(robot_interface::robot_interface PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ROBOT_INTERFACE_INCLUDE_DIR}")

add_library(cia402::cia402 SHARED IMPORTED GLOBAL)
set_target_properties(cia402::cia402 PROPERTIES
    IMPORTED_LOCATION "${CIA402_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CIA402_INCLUDE_DIR}")

add_subdirectory(src)
add_subdirectory(GSD620)
add_subdirectory(SIASUN)

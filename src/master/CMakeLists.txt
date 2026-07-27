# IgH 通用核心：只包含主站生命周期、Domain 周期收发和从站抽象接口。
add_library(igh_core STATIC
    igh_master.cpp
)
add_library(igh::core ALIAS igh_core)

target_include_directories(igh_core
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${ETHERCAT_INCLUDE_DIR}"
)

target_link_libraries(igh_core
    PUBLIC
    "${ETHERCAT_LIBRARY}"
)

target_compile_options(igh_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

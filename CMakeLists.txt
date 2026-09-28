cmake_minimum_required(VERSION 3.16)
project(dpdk-forwarder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# DPDK 通过 pkg-config 查找（libdpdk.pc 由 DPDK 的 meson 构建安装）
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${DPDK_INCLUDE_DIRS}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -O3 -g -Wall -Wextra
    ${DPDK_CFLAGS_OTHER}
)

target_link_directories(${PROJECT_NAME} PRIVATE ${DPDK_LIBRARY_DIRS})

# openEuler / 部分发行版打包的 libdpdk.pc 把 bus 驱动（librte_bus_pci 等）
# 放在了 Libs.private 里，动态链接时不会带上。后果是程序能编过，但 EAL
# 启动时 PCI bus 没注册，`-a 0000:xx:xx.x` 这类设备参数会解析失败：
#     EAL: failed to parse device "0000:02:00.1"
# 这里显式补上要用到的 bus 库（重复链接同一库是无害的）。
foreach(_bus rte_bus_pci rte_bus_vdev)
    find_library(DPDK_${_bus}_LIB ${_bus} HINTS ${DPDK_LIBRARY_DIRS})
    if(DPDK_${_bus}_LIB)
        list(APPEND DPDK_EXTRA_LIBS ${DPDK_${_bus}_LIB})
    endif()
endforeach()

target_link_libraries(${PROJECT_NAME} PRIVATE ${DPDK_LIBRARIES} ${DPDK_EXTRA_LIBS})

# ---- 单元测试 ----
option(BUILD_TESTS "构建单元测试" ON)

if(BUILD_TESTS)
    enable_testing()

    add_executable(test_basic
        tests/test_basic.cpp
        src/acl.cpp
        src/mac_table.cpp
        src/packet.cpp
    )

    target_include_directories(test_basic PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${DPDK_INCLUDE_DIRS}
    )

    # 必须带上 DPDK 的编译选项：rte_hash_crc 用到 SSE4.2 内建函数，
    # 少了 -msse4.2 会报 "target specific option mismatch"
    target_compile_options(test_basic PRIVATE
        -O2 -g -Wall -Wextra
        ${DPDK_CFLAGS_OTHER}
    )
    target_link_directories(test_basic PRIVATE ${DPDK_LIBRARY_DIRS})
    target_link_libraries(test_basic PRIVATE ${DPDK_LIBRARIES} ${DPDK_EXTRA_LIBS})

    add_test(NAME basic COMMAND test_basic)
endif()

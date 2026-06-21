# ButterflyBlades - CMake Build Configuration
# Cross-platform: Windows / Linux / macOS

cmake_minimum_required(VERSION 3.20)
project(ButterflyBlades VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: 强制 UTF-8 源码编码，避免代码页 936 导致中文注释破坏解析
if(MSVC)
    add_compile_options(/utf-8 /bigobj /MP)
endif()

# ========== 编译选项 ==========
option(BB_BUILD_DRIVER "Build kernel driver component (Windows only)" OFF)
option(BB_BUILD_CONSOLE "Build remote console client" ON)
option(BB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(BB_STATIC_MODULES "Statically link all modules into main binary" ON)

if(BB_ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

# ========== 平台检测 ==========
if(WIN32)
    set(PLATFORM_SOURCES
        src/platform/windows/windows_platform.cpp
    )
    set(PLATFORM_LIBS
        ws2_32 iphlpapi crypt32 psapi gdi32
    )
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_SOURCES
        src/platform/linux/linux_platform.cpp
    )
    set(PLATFORM_LIBS pthread dl)
elseif(APPLE)
    set(PLATFORM_SOURCES
        src/platform/linux/linux_platform.cpp  # macOS stub
    )
    set(PLATFORM_LIBS pthread dl)
endif()

# ========== 头文件目录 ==========
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/common
    ${CMAKE_SOURCE_DIR}/include/core
    ${CMAKE_SOURCE_DIR}/include/modules
)

# ========== JSON 库 (nlohmann - 本地单头文件) ==========
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party/nlohmann)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

# ========== 核心库 ==========
set(CORE_SOURCES
    src/core/module_manager.cpp
)

add_library(bb_core STATIC ${CORE_SOURCES})
target_link_libraries(bb_core PUBLIC nlohmann_json::nlohmann_json)
target_include_directories(bb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ========== 模块源码 ==========
set(MODULE_SOURCES
    src/modules/process_monitor/process_monitor.cpp
    src/modules/thread_monitor/thread_monitor.cpp
    src/modules/integrity_check/integrity_check.cpp
    src/modules/virus_scan/virus_scan.cpp
    src/modules/popup_blocker/popup_blocker.cpp
    src/modules/self_protect/self_protect.cpp
    src/modules/remote_console/remote_console.cpp
)

if(BB_STATIC_MODULES)
    add_compile_definitions(BB_STATIC_MODULES)
    # 所有模块静态链接到主程序
    set(BB_SOURCES
        src/core/main.cpp
        ${CORE_SOURCES}
        ${MODULE_SOURCES}
        ${PLATFORM_SOURCES}
    )

    add_executable(ButterflyBlades ${BB_SOURCES})
    target_link_libraries(ButterflyBlades PRIVATE nlohmann_json::nlohmann_json ${PLATFORM_LIBS})

    if(WIN32)
        set_target_properties(ButterflyBlades PROPERTIES
            WIN32_EXECUTABLE FALSE  # 控制台应用，方便调试
        )
    endif()
else()
    # 主程序 + 动态模块加载
    add_executable(ButterflyBlades
        src/core/main.cpp
        ${CORE_SOURCES}
        ${PLATFORM_SOURCES}
    )
    target_link_libraries(ButterflyBlades PRIVATE nlohmann_json::nlohmann_json ${PLATFORM_LIBS})

    # 各模块编译为独立的 DLL/SO
    set(MODULE_NAMES
        process_monitor thread_monitor integrity_check
        virus_scan popup_blocker self_protect remote_console
    )

    foreach(MOD_NAME ${MODULE_NAMES})
        add_library(bb_${MOD_NAME} SHARED
            src/modules/${MOD_NAME}/${MOD_NAME}.cpp
        )
        target_link_libraries(bb_${MOD_NAME} PRIVATE bb_core nlohmann_json::nlohmann_json)
        target_include_directories(bb_${MOD_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)

        if(WIN32)
            set_target_properties(bb_${MOD_NAME} PROPERTIES PREFIX "")
        endif()
    endforeach()
endif()

# ========== 安装 ==========
install(TARGETS ButterflyBlades DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/ButterflyBlades/config)
install(DIRECTORY scripts/ DESTINATION share/ButterflyBlades/scripts)

# ========== 远程控制台客户端 ==========
if(BB_BUILD_CONSOLE)
    add_executable(bb_console
        console/console_client.cpp
    )
    target_link_libraries(bb_console PRIVATE nlohmann_json::nlohmann_json ${PLATFORM_LIBS})
    install(TARGETS bb_console DESTINATION bin)
endif()

# ========== 内核驱动 (Windows) ==========
if(WIN32 AND BB_BUILD_DRIVER)
    message(STATUS "Kernel driver build enabled. Requires WDK.")
    # 驱动需使用单独的 WDK CMake 项目，此处只做标记
endif()

# 打印构建摘要
message(STATUS "========================================")
message(STATUS "ButterflyBlades Build Configuration")
message(STATUS "  Version:    ${PROJECT_VERSION}")
message(STATUS "  Platform:   ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Static:     ${BB_STATIC_MODULES}")
message(STATUS "  ASan:       ${BB_ENABLE_ASAN}")
message(STATUS "========================================")

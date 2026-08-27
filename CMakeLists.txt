cmake_minimum_required(VERSION 3.22.1)
project("Terraria-KernelLoader-Mods")
set(CMAKE_C_STANDARD 11)

# 打印调试信息
message(STATUS "=== SYSTEM INFO ===")
message(STATUS "CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_HOST_SYSTEM_PROCESSOR: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_SIZEOF_VOID_P: ${CMAKE_SIZEOF_VOID_P}")
message(STATUS "=========================")

include_directories(${CMAKE_SOURCE_DIR}/mod-api)

macro(add_terraria_mod MOD_NAME)
    message(STATUS "Adding Terraria Mod: ${MOD_NAME}")

    # 1. 创建库
    add_library(${MOD_NAME} SHARED ${CMAKE_SOURCE_DIR}/mod-api/tefkernel/tef_api_imp.c ${ARGN})

    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
            add_custom_command(TARGET ${MOD_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Stripping symbols from ${MOD_NAME}..."
                COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:${MOD_NAME}>
                COMMENT "Stripping symbols from ${MOD_NAME}library using strip command"
                VERBATIM
            )
        elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            # macOS 使用不同的 strip 选项
            add_custom_command(TARGET ${MOD_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "Stripping symbols from ${MOD_NAME}..."
                COMMAND ${CMAKE_STRIP} -S $<TARGET_FILE:${MOD_NAME}>
                COMMENT "Stripping symbols from ${MOD_NAME}library using strip command"
                VERBATIM
            )
        endif()
    endif()

    # 2. 系统名称
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(LOCAL_SYSTEM "linux")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(LOCAL_SYSTEM "darwin")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(LOCAL_SYSTEM "windows")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(LOCAL_SYSTEM "android")
    else()
        set(LOCAL_SYSTEM "unknown")
    endif()

    # 3. 架构检测（重点修复）
    if(CMAKE_SYSTEM_PROCESSOR)
        # 有 CMAKE_SYSTEM_PROCESSOR
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set(LOCAL_ARCH "x64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686|i386|x86")
            set(LOCAL_ARCH "x86")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(LOCAL_ARCH "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|armv7a|ARMv7|arm")
            set(LOCAL_ARCH "arm")
        else()
            set(LOCAL_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
    else()
        # 回退方案
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(LOCAL_ARCH "x64")
        else()
            set(LOCAL_ARCH "x86")
        endif()
    endif()

    # 4. 输出文件名
    if(WIN32)
        set_target_properties(${MOD_NAME} PROPERTIES
                OUTPUT_NAME "${MOD_NAME}.${LOCAL_SYSTEM}.${LOCAL_ARCH}"
                SUFFIX ".dll"
                PREFIX ""
        )
    elseif(APPLE)
        set_target_properties(${MOD_NAME} PROPERTIES
                OUTPUT_NAME "${MOD_NAME}.${LOCAL_SYSTEM}.${LOCAL_ARCH}"
                SUFFIX ".dylib"
        )
    else()
        set_target_properties(${MOD_NAME} PROPERTIES
                OUTPUT_NAME "${MOD_NAME}.${LOCAL_SYSTEM}.${LOCAL_ARCH}"
                SUFFIX ".so"
        )
    endif()

    message(STATUS "  -> Output: ${MOD_NAME}.${LOCAL_SYSTEM}.${LOCAL_ARCH}")

    # 包含 API
    target_include_directories(${MOD_NAME}
            PRIVATE ${CMAKE_SOURCE_DIR}/mod-api
    )
endmacro()

add_subdirectory(ResourceSaver)

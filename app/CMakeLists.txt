cmake_minimum_required(VERSION 3.13)

# Switube — Native YouTube Music client for Nintendo Switch
# Build: .\build-app.ps1 (PLATFORM_SWITCH)

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/borealis/library)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(Switube)

if (PLATFORM_SWITCH)
    add_compile_definitions(__SWITCH__)
    add_compile_options(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec)
endif ()

set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")
set(VERSION_ALTER "0")
set(PROJECT_AUTHOR "Switube Team")
set(PROJECT_TITLEID "010ff000ffff0120")
set(PACKAGE_NAME "app.switube")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/bell/external/nlohmann_json/include
)

# --- Borealis UI Framework
add_subdirectory(${BOREALIS_LIBRARY} ${CMAKE_BINARY_DIR}/borealis)

# --- App sources
file(GLOB MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(APPEND MAIN_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/NXConsoleLog.cpp
)
if (PLATFORM_SWITCH)
    list(APPEND MAIN_SRC
        ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/NXAudioSink.cpp
    )
endif ()

program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/bell/external/nlohmann_json/include
)

target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(${PROJECT_NAME} PRIVATE borealis)

if (PLATFORM_SWITCH)
    find_package(CURL REQUIRED)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CURL_STATICLIB)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${CURL_LIBRARIES}
        avformat
        avcodec
        swresample
        avutil
        bz2
        mbedtls
        mbedx509
        mbedcrypto
        dav1d
        z
        m
    )
endif ()

# --- NRO packaging
if (PLATFORM_SWITCH)
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif ()

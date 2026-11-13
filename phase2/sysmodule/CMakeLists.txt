cmake_minimum_required(VERSION 3.13)

project(switube-background C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "Use the devkitPro Nintendo Switch toolchain")
endif()

add_compile_definitions(__SWITCH__)
add_compile_options(
  -march=armv8-a+crc+crypto
  -mtune=cortex-a57
  -mtp=soft
  -ftls-model=local-exec
  -ffunction-sections
  -fdata-sections
  -Os
  -flto
)
add_link_options(-Wl,--gc-sections -flto)

find_library(AVUTIL_LIB avutil REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(TX_FLOAT_RAW ${CMAKE_CURRENT_BINARY_DIR}/tx_float.o)
set(TX_FLOAT_SLIM ${CMAKE_CURRENT_BINARY_DIR}/tx_float_slim.o)

add_custom_command(
  OUTPUT ${TX_FLOAT_SLIM}
  COMMAND ${CMAKE_AR} x ${AVUTIL_LIB} tx_float.o
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/slim_tx_float.py
          ${TX_FLOAT_RAW} ${TX_FLOAT_SLIM}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${AVUTIL_LIB} ${CMAKE_CURRENT_SOURCE_DIR}/slim_tx_float.py
  VERBATIM
)

add_executable(switube-background
  engine/main.cpp
  engine/background_backend.cpp
  source/ipc_server.c
  source/ipc_self_test.cpp
  source/switube_service.cpp
  source/ffmpeg_stubs.c
  source/ffmpeg_custom_list.c
  ${TX_FLOAT_SLIM}
  ../common/source/service_core.cpp
  ../../app/src/yt_engine.cpp
  ../../app/src/yt_audio_player.cpp
  ../../app/src/yt_music_client.cpp
  ../../app/src/i18n.cpp
  ../../targets/switch/NXAudioSink.cpp
  ../../targets/switch/NXConsoleLog.cpp
)

target_include_directories(switube-background PRIVATE
  engine
  source
  ../common/include
  ../../app/src
  ../../targets/switch
  ../../vendor/bell/external/nlohmann_json/include
)

find_package(CURL REQUIRED)
target_compile_definitions(switube-background PRIVATE
  CURL_STATICLIB
  SWITUBE_BACKGROUND_SERVICE
)
target_include_directories(switube-background PRIVATE ${CURL_INCLUDE_DIRS})

target_link_libraries(switube-background PRIVATE
  ${CURL_LIBRARIES}
  avformat
  avcodec
  swresample
  avutil
  bz2
  z
  m
  nx
)

find_program(NPDMTOOL npdmtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(ELF2NSO elf2nso HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(BUILD_PFS0 build_pfs0 HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)

set(PACKAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
set(PACKAGE_NPDM ${CMAKE_CURRENT_BINARY_DIR}/switube-background.npdm)
set(PACKAGE_NSO ${CMAKE_CURRENT_BINARY_DIR}/switube-background.nso)
set(PACKAGE_NSP ${CMAKE_CURRENT_BINARY_DIR}/switube-background.nsp)

add_custom_command(
  OUTPUT ${PACKAGE_NPDM}
  COMMAND ${NPDMTOOL} ${CMAKE_CURRENT_SOURCE_DIR}/switube-sysmodule.json
          ${PACKAGE_NPDM}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/switube-sysmodule.json
  VERBATIM
)

add_custom_command(
  OUTPUT ${PACKAGE_NSO}
  COMMAND ${ELF2NSO} $<TARGET_FILE:switube-background> ${PACKAGE_NSO}
  DEPENDS switube-background
  VERBATIM
)

add_custom_command(
  OUTPUT ${PACKAGE_NSP}
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PACKAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${PACKAGE_NSO} ${PACKAGE_DIR}/main
  COMMAND ${CMAKE_COMMAND} -E copy ${PACKAGE_NPDM} ${PACKAGE_DIR}/main.npdm
  COMMAND ${BUILD_PFS0} ${PACKAGE_DIR} ${PACKAGE_NSP}
  DEPENDS ${PACKAGE_NSO} ${PACKAGE_NPDM}
  VERBATIM
)

add_custom_target(switube-background-package ALL DEPENDS ${PACKAGE_NSP})

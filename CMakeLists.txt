# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(b2b_core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(B2B_WITH_AUDIO "Build the demo with real audio output (needs third_party/miniaudio/miniaudio.h)" ON)
# When b2b-core is pulled into another project (e.g. the Mixxx fork) only the library is built.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(B2B_TOP_LEVEL ON)
else()
  set(B2B_TOP_LEVEL OFF)
endif()
option(B2B_BUILD_DEMO "Build the metronome demo and tests" ${B2B_TOP_LEVEL})

find_package(Threads REQUIRED)

# ---- The core library: everything that would later plug into Mixxx ----------
add_library(b2b_core
  src/clock_sync.cpp
  src/crypto.cpp
  src/engine.cpp
  src/event_log.cpp
  src/file_share.cpp
  src/net.cpp
  src/protocol.cpp
  src/report.cpp
  src/session.cpp
  third_party/kcp/ikcp.c
  third_party/monocypher/monocypher.c
)
target_include_directories(b2b_core PRIVATE third_party/kcp third_party/monocypher)
# Third-party code, kept unmodified: don't show its warnings.
set_source_files_properties(third_party/kcp/ikcp.c third_party/monocypher/monocypher.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
target_include_directories(b2b_core PUBLIC include)
target_link_libraries(b2b_core PUBLIC Threads::Threads)
# Where join codes come from (b2b-server). Override at run time with B2B_SERVER.
set(B2B_DEFAULT_SERVER "connect.pitchride.co.uk:7700" CACHE STRING "Join-code server, host:port")
# Its address, for when its name can't be looked up.
set(B2B_FALLBACK_HOST "178.104.233.96" CACHE STRING "The join-code server's address, if its name isn't found")
string(REGEX REPLACE ":[0-9]+$" "" B2B_DEFAULT_HOST "${B2B_DEFAULT_SERVER}")
target_compile_definitions(b2b_core PRIVATE B2B_DEFAULT_SERVER="${B2B_DEFAULT_SERVER}"
  B2B_DEFAULT_HOST="${B2B_DEFAULT_HOST}" B2B_FALLBACK_HOST="${B2B_FALLBACK_HOST}")
if(WIN32)
  target_link_libraries(b2b_core PUBLIC ws2_32 bcrypt)
endif()
if(MSVC)
  target_compile_options(b2b_core PRIVATE /W3)
else()
  target_compile_options(b2b_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(b2b_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Optional Qt user interface (menu, dialogs, status strip) -----------------
# Plain Qt widgets, independent of any DJ software. Built when Qt is found.
option(B2B_WITH_QT "Build the Qt user interface module (b2b_qt)" ON)
if(B2B_WITH_QT)
  find_package(Qt6 COMPONENTS Widgets Network QUIET)
  if(Qt6_FOUND)
    set(B2B_QT Qt6)
  else()
    find_package(Qt5 COMPONENTS Widgets Network QUIET)
    if(Qt5_FOUND)
      set(B2B_QT Qt5)
    endif()
  endif()
  if(B2B_QT)
    add_library(b2b_qt
      qt/src/session_ui.cpp
      qt/include/b2b/qt/session_ui.hpp
    )
    set_target_properties(b2b_qt PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
    target_include_directories(b2b_qt PUBLIC qt/include)
    target_link_libraries(b2b_qt PUBLIC b2b_core ${B2B_QT}::Widgets ${B2B_QT}::Network)
    message(STATUS "b2b: Qt user interface enabled (${B2B_QT})")
  else()
    message(STATUS "b2b: Qt not found, user interface module disabled")
  endif()
endif()

if(NOT B2B_BUILD_DEMO)
  return()
endif()

# ---- Demo: two-peer metronome --------------------------------------------------
add_executable(b2b_demo demo/b2b_demo.cpp demo/audio_output.cpp)
target_link_libraries(b2b_demo PRIVATE b2b_core)

set(MINIAUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
if(B2B_WITH_AUDIO AND EXISTS ${MINIAUDIO_DIR}/miniaudio.h)
  message(STATUS "b2b: audio output enabled (miniaudio)")
  target_compile_definitions(b2b_demo PRIVATE B2B_HAVE_AUDIO)
  target_include_directories(b2b_demo SYSTEM PRIVATE ${MINIAUDIO_DIR})
  target_link_libraries(b2b_demo PRIVATE ${CMAKE_DL_LIBS} m)
else()
  message(STATUS "b2b: audio output disabled (run tools/fetch_miniaudio.sh to enable)")
endif()

# ---- Tests ---------------------------------------------------------------------
enable_testing()
add_executable(b2b_tests tests/test_core.cpp)
target_link_libraries(b2b_tests PRIVATE b2b_core)
add_test(NAME b2b_tests COMMAND b2b_tests)

add_executable(b2b_file_share_tests tests/test_file_share.cpp)
target_link_libraries(b2b_file_share_tests PRIVATE b2b_core)
add_test(NAME b2b_file_share_tests COMMAND b2b_file_share_tests)

add_executable(b2b_session_tests tests/test_session.cpp)
target_link_libraries(b2b_session_tests PRIVATE b2b_core)
add_test(NAME b2b_session_tests COMMAND b2b_session_tests)

# Not a test: how fast a track reaches the friend on simulated networks.
add_executable(b2b_bench_share tools/bench_share.cpp)
target_link_libraries(b2b_bench_share PRIVATE b2b_core)

add_executable(b2b_report_tests tests/test_report.cpp)
target_link_libraries(b2b_report_tests PRIVATE b2b_core)
add_test(NAME b2b_report_tests COMMAND b2b_report_tests)

# Needs a rendezvous server: tests/rendezvous_test.sh starts one from b2b-server.
add_executable(b2b_rendezvous_tests tests/test_rendezvous.cpp)
target_link_libraries(b2b_rendezvous_tests PRIVATE b2b_core)

cmake_minimum_required(VERSION 3.28)
project(studio-windows-audio-checks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# These probes invoke actual widgets without QtTest. Keep their configuration
# separate from the other suites, whose QtTest/Svg dependencies are optional
# in the OBS Windows prebuilt dependency bundle.
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

get_filename_component(STUDIO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(studio_check IN ITEMS audio-cue-vod-windows audio-vod-active-windows)
  set(studio_target "studio-${studio_check}-check")
  add_library(
    ${studio_target} MODULE
    "../${studio_check}-check.cpp"
    "${STUDIO_SOURCE_ROOT}/frontend/utility/MultiStreamManager.cpp"
    "${STUDIO_SOURCE_ROOT}/frontend/utility/MultistreamChannelPlan.cpp"
    "${STUDIO_SOURCE_ROOT}/frontend/utility/StreamPlatform.cpp"
  )
  target_include_directories(${studio_target} PRIVATE "${STUDIO_SOURCE_ROOT}/frontend")
  target_link_libraries(${studio_target} PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
  set_target_properties(${studio_target} PROPERTIES PREFIX "")
  if(WIN32)
    target_compile_definitions(${studio_target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  endif()
  if(MSVC)
    target_compile_options(${studio_target} PRIVATE /utf-8)
  endif()
endforeach()

# Same source on Linux/Windows; no private frontend C++ symbols or QtTest.
add_library(studio-profile-defaults-check MODULE ../profile-defaults-check.cpp)
target_link_libraries(studio-profile-defaults-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-profile-defaults-check PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(studio-profile-defaults-check PRIVATE /utf-8)
endif()

add_library(studio-windows-outputs-followup-check MODULE outputs-followup-check.cpp
  "${STUDIO_SOURCE_ROOT}/frontend/utility/MultiStreamManager.cpp"
  "${STUDIO_SOURCE_ROOT}/frontend/utility/MultistreamChannelPlan.cpp"
  "${STUDIO_SOURCE_ROOT}/frontend/utility/StreamPlatform.cpp")
target_include_directories(studio-windows-outputs-followup-check PRIVATE "${STUDIO_SOURCE_ROOT}/frontend")
target_link_libraries(studio-windows-outputs-followup-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-windows-outputs-followup-check PROPERTIES PREFIX "")
option(STUDIO_WINDOWS_PRIVATE_CLOCK_OBSERVER "Read-only QA observer, exact matching private Windows core headers only" OFF)
set(STUDIO_WINDOWS_CORE_BUILD_DIR "" CACHE PATH "Build directory of the exact running QA libobs")
if(STUDIO_WINDOWS_PRIVATE_CLOCK_OBSERVER)
  if(NOT WIN32 OR NOT EXISTS "${STUDIO_WINDOWS_CORE_BUILD_DIR}/config/obsconfig.h")
    message(FATAL_ERROR "Private observer requires explicit matching Windows core build directory")
  endif()
  find_library(STUDIO_WINDOWS_PTHREAD_IMPORT NAMES w32-pthreads
    PATHS "${STUDIO_WINDOWS_CORE_BUILD_DIR}/deps/w32-pthreads/RelWithDebInfo" NO_DEFAULT_PATH REQUIRED)
  target_sources(studio-windows-outputs-followup-check PRIVATE output-clock-observer.c)
  set_target_properties(studio-windows-outputs-followup-check PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
  target_compile_definitions(studio-windows-outputs-followup-check PRIVATE STUDIO_CLOCK_PRIVATE_OBSERVER=1)
  target_include_directories(studio-windows-outputs-followup-check PRIVATE
    "${STUDIO_SOURCE_ROOT}/libobs" "${STUDIO_SOURCE_ROOT}/deps/w32-pthreads"
    "${STUDIO_SOURCE_ROOT}/deps/libcaption" "${STUDIO_WINDOWS_CORE_BUILD_DIR}/config")
  target_link_libraries(studio-windows-outputs-followup-check PRIVATE "${STUDIO_WINDOWS_PTHREAD_IMPORT}")
endif()

if(MSVC)
 target_compile_definitions(studio-windows-outputs-followup-check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
 target_compile_options(studio-windows-outputs-followup-check PRIVATE /utf-8)
endif()

add_library(studio-monitoring-device-check MODULE ../monitoring-device-check.cpp)
target_link_libraries(studio-monitoring-device-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-monitoring-device-check PROPERTIES PREFIX "")
add_executable(studio-style-check ../../studio-style/canvas-card-style-check.cpp)
target_include_directories(studio-style-check PRIVATE "${STUDIO_SOURCE_ROOT}/frontend")
target_link_libraries(studio-style-check PRIVATE Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-monitoring-device-check PRIVATE /utf-8)
 target_compile_options(studio-style-check PRIVATE /utf-8)
endif()

add_library(studio-windows-canvas-card-followup-check MODULE canvas-card-followup-check.cpp)
target_link_libraries(studio-windows-canvas-card-followup-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-windows-canvas-card-followup-check PROPERTIES PREFIX "")
if(MSVC)
 target_compile_options(studio-windows-canvas-card-followup-check PRIVATE /utf-8)
endif()

add_executable(studio-chat-provider-errors-check ../chat-provider-errors-check.cpp
  "${STUDIO_SOURCE_ROOT}/frontend/chat/YouTubeChatConnection.cpp"
  "${STUDIO_SOURCE_ROOT}/frontend/chat/YouTubeChatConnection.hpp"
  "${STUDIO_SOURCE_ROOT}/frontend/chat/ChatConnection.cpp"
  "${STUDIO_SOURCE_ROOT}/frontend/chat/ChatConnection.hpp")
set_target_properties(studio-chat-provider-errors-check PROPERTIES AUTOMOC ON)
target_include_directories(studio-chat-provider-errors-check PRIVATE "${STUDIO_SOURCE_ROOT}/frontend")
target_link_libraries(studio-chat-provider-errors-check PRIVATE Qt6::Core Qt6::Network)
if(MSVC)
 target_compile_options(studio-chat-provider-errors-check PRIVATE /utf-8)
endif()

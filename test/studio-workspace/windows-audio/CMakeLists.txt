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

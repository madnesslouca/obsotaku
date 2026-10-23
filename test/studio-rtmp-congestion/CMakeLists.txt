cmake_minimum_required(VERSION 3.28)
project(studio-rtmp-congestion LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
add_library(studio-rtmp-congestion-check MODULE media-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-rtmp-congestion-check PRIVATE ../../frontend ../../frontend/utility)
target_link_libraries(studio-rtmp-congestion-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-rtmp-congestion-check PROPERTIES PREFIX "")
if(MSVC)
 target_compile_options(studio-rtmp-congestion-check PRIVATE /utf-8)
 target_compile_definitions(studio-rtmp-congestion-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

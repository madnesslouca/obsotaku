cmake_minimum_required(VERSION 3.28)
project(multistream-cancel-check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/finders")
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
set(STUDIO_MANAGER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../../frontend/utility/MultiStreamManager.cpp" CACHE FILEPATH "Manager under test")
add_executable(multistream-cancel-check check.cpp "${STUDIO_MANAGER_SOURCE}"
  ../../../frontend/utility/MultistreamChannelPlan.cpp
  ../../../frontend/utility/StreamPlatform.cpp)
target_include_directories(multistream-cancel-check PRIVATE ../../../frontend)
target_include_directories(multistream-cancel-check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../frontend/utility")
target_link_libraries(multistream-cancel-check PRIVATE Qt6::Core OBS::libobs)
add_library(multistream-cancel-media-check MODULE ../multistream-reconnect-check.cpp
  "${STUDIO_MANAGER_SOURCE}"
  ../../../frontend/utility/MultistreamChannelPlan.cpp
  ../../../frontend/utility/StreamPlatform.cpp)
target_include_directories(multistream-cancel-media-check PRIVATE ../../../frontend ../../../frontend/utility)
target_link_libraries(multistream-cancel-media-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(multistream-cancel-media-check PROPERTIES PREFIX "")

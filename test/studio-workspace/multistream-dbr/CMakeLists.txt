cmake_minimum_required(VERSION 3.28)
project(multistream-dbr-check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/finders")
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(libobs REQUIRED)
set(STUDIO_MANAGER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../../frontend/utility/MultiStreamManager.cpp" CACHE FILEPATH "Manager under test")
option(STUDIO_DBR_SNAPSHOT "Check new policy snapshot contract" OFF)
add_executable(multistream-dbr-check check.cpp "${STUDIO_MANAGER_SOURCE}"
  ../../../frontend/utility/MultistreamChannelPlan.cpp
  ../../../frontend/utility/StreamPlatform.cpp)
target_include_directories(multistream-dbr-check PRIVATE ../../../frontend ../../../frontend/utility)
target_link_libraries(multistream-dbr-check PRIVATE Qt6::Core OBS::libobs)
if(STUDIO_DBR_SNAPSHOT)
  target_compile_definitions(multistream-dbr-check PRIVATE STUDIO_DBR_SNAPSHOT)
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(obs-frontend-api REQUIRED)
add_library(multistream-dbr-media-check MODULE media-check.cpp "${STUDIO_MANAGER_SOURCE}"
  ../../../frontend/utility/MultistreamChannelPlan.cpp
  ../../../frontend/utility/StreamPlatform.cpp)
target_include_directories(multistream-dbr-media-check PRIVATE ../../../frontend ../../../frontend/utility)
target_link_libraries(multistream-dbr-media-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(multistream-dbr-media-check PROPERTIES PREFIX "")

if(MSVC)
 target_compile_options(multistream-dbr-check PRIVATE /utf-8)
 target_compile_definitions(multistream-dbr-check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

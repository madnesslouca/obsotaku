cmake_minimum_required(VERSION 3.28)
project(studio-encoder-shared-stop LANGUAGES C)
set(CMAKE_C_STANDARD 17)
find_package(libobs REQUIRED)
find_package(Threads REQUIRED)
set(STUDIO_TEST_CORE "" CACHE FILEPATH "Exact core library under test")
if(NOT EXISTS "${STUDIO_TEST_CORE}")
  message(FATAL_ERROR "Set STUDIO_TEST_CORE to the exact baseline or candidate core")
endif()
add_executable(studio-encoder-shared-stop-check check.c)
target_include_directories(studio-encoder-shared-stop-check PRIVATE
  ../../libobs ../../deps/libcaption
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(studio-encoder-shared-stop-check PRIVATE
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
target_link_libraries(studio-encoder-shared-stop-check PRIVATE "${STUDIO_TEST_CORE}"
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES> Threads::Threads)
if(MSVC)
  target_compile_options(studio-encoder-shared-stop-check PRIVATE /utf-8)
  target_compile_definitions(studio-encoder-shared-stop-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

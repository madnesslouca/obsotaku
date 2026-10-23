cmake_minimum_required(VERSION 3.28)
project(studio-slideshow-edges LANGUAGES C)
set(CMAKE_C_STANDARD 17)
find_package(libobs REQUIRED)
find_package(Threads REQUIRED)
set(STUDIO_TEST_CORE "" CACHE FILEPATH "Private active-graph core with graph QA exports")
set(STUDIO_SLIDESHOW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/image-source" CACHE PATH "Exact slideshow C sources under test")
if(NOT EXISTS "${STUDIO_TEST_CORE}")
 message(FATAL_ERROR "A private graph adapter core is mandatory; never patch an installed library")
endif()
add_executable(studio-slideshow-edges-check check.c
 "${STUDIO_SLIDESHOW_SOURCE_DIR}/obs-slideshow.c"
 "${STUDIO_SLIDESHOW_SOURCE_DIR}/obs-slideshow-mk2.c")
target_include_directories(studio-slideshow-edges-check PRIVATE
 $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(studio-slideshow-edges-check PRIVATE
 $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
target_link_libraries(studio-slideshow-edges-check PRIVATE "${STUDIO_TEST_CORE}"
 $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES> Threads::Threads $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
if(MSVC)
 target_compile_options(studio-slideshow-edges-check PRIVATE /utf-8)
 target_compile_definitions(studio-slideshow-edges-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(STUDIO_SLIDESHOW_LIFETIME_SOURCE_DIR "" CACHE PATH "Optional instrumented source copy for deterministic lifetime fixture")
if(STUDIO_SLIDESHOW_LIFETIME_SOURCE_DIR)
 add_executable(studio-slideshow-lifetime-check lifetime-check.c
  "${STUDIO_SLIDESHOW_LIFETIME_SOURCE_DIR}/obs-slideshow.c"
  "${STUDIO_SLIDESHOW_LIFETIME_SOURCE_DIR}/obs-slideshow-mk2.c")
 target_include_directories(studio-slideshow-lifetime-check PRIVATE $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
 target_compile_definitions(studio-slideshow-lifetime-check PRIVATE $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
 target_link_libraries(studio-slideshow-lifetime-check PRIVATE "${STUDIO_TEST_CORE}"
  $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES> Threads::Threads $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
 if(MSVC)
  target_compile_options(studio-slideshow-lifetime-check PRIVATE /utf-8)
  target_compile_definitions(studio-slideshow-lifetime-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
 endif()
endif()

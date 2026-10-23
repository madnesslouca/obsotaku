cmake_minimum_required(VERSION 3.28)
project(studio_active_graph_checks LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

set(STUDIO_GRAPH_CORE_SOURCE_DIR "" CACHE PATH "Matching external OBS SDK source tree")
set(STUDIO_GRAPH_CORE_LIBRARY "" CACHE FILEPATH "Exact test libobs shared library or Windows import library")
set(STUDIO_GRAPH_CORE_CONFIG_INCLUDE_DIR "" CACHE PATH "Matching SDK build/config directory")
set(STUDIO_GRAPH_CORE_GENERATED_INCLUDE_DIR "" CACHE PATH "Matching SDK build/libobs directory")
set(STUDIO_GRAPH_TRANSITION_SOURCE_DIR "${STUDIO_GRAPH_CORE_SOURCE_DIR}/plugins/obs-transitions"
 CACHE PATH "External SDK directory containing Cut and Fade implementations")
option(STUDIO_GRAPH_BUILD_COST "Build the optional layered-DAG cost witness" ON)

foreach(required STUDIO_GRAPH_CORE_SOURCE_DIR STUDIO_GRAPH_CORE_LIBRARY
 STUDIO_GRAPH_CORE_CONFIG_INCLUDE_DIR STUDIO_GRAPH_CORE_GENERATED_INCLUDE_DIR
 STUDIO_GRAPH_TRANSITION_SOURCE_DIR)
 if(NOT EXISTS "${${required}}")
  message(FATAL_ERROR "Set ${required} to an existing matching SDK artifact")
 endif()
endforeach()
foreach(required_file "${STUDIO_GRAPH_CORE_SOURCE_DIR}/libobs/obs.h"
 "${STUDIO_GRAPH_CORE_SOURCE_DIR}/libobs/obs-internal.h"
 "${STUDIO_GRAPH_CORE_SOURCE_DIR}/libobs/obs-scene.h"
 "${STUDIO_GRAPH_CORE_SOURCE_DIR}/deps/libcaption/caption/caption.h"
 "${STUDIO_GRAPH_CORE_CONFIG_INCLUDE_DIR}/obsconfig.h"
 "${STUDIO_GRAPH_TRANSITION_SOURCE_DIR}/transition-cut.c"
 "${STUDIO_GRAPH_TRANSITION_SOURCE_DIR}/transition-fade.c")
 if(NOT EXISTS "${required_file}")
  message(FATAL_ERROR "Matching SDK input is missing: ${required_file}")
 endif()
endforeach()


# Record actual SDK inputs without pinning this reusable suite to one release.
set(sdk_manifest "{}")
foreach(relative_path libobs/obs.h libobs/obs-internal.h libobs/obs-scene.h
 libobs/obs-source.c libobs/obs-scene.c)
 file(SHA256 "${STUDIO_GRAPH_CORE_SOURCE_DIR}/${relative_path}" source_hash)
 string(JSON sdk_manifest SET "${sdk_manifest}" "${relative_path}" "\"${source_hash}\"")
endforeach()
foreach(file_name transition-cut.c transition-fade.c)
 file(SHA256 "${STUDIO_GRAPH_TRANSITION_SOURCE_DIR}/${file_name}" source_hash)
 string(JSON sdk_manifest SET "${sdk_manifest}" "${file_name}" "\"${source_hash}\"")
endforeach()
file(SHA256 "${STUDIO_GRAPH_CORE_LIBRARY}" library_hash)
string(JSON sdk_manifest SET "${sdk_manifest}" "core_link_library" "\"${library_hash}\"")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk-manifest.json" "${sdk_manifest}\n")

# The SDK package supplies transitive SIMDe/pthread requirements. The explicit
# library below selects the test core; no installed OBS runtime is linked here.
find_package(libobs CONFIG REQUIRED)
find_package(Threads REQUIRED)
set(sdk_includes "${STUDIO_GRAPH_CORE_SOURCE_DIR}" "${STUDIO_GRAPH_CORE_SOURCE_DIR}/libobs"
 "${STUDIO_GRAPH_CORE_SOURCE_DIR}/deps/libcaption" "${STUDIO_GRAPH_CORE_CONFIG_INCLUDE_DIR}"
 "${STUDIO_GRAPH_CORE_GENERATED_INCLUDE_DIR}")

function(add_graph_check target)
 add_executable(${target} ${ARGN})
 target_include_directories(${target} BEFORE PRIVATE ${sdk_includes}
   $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
 target_compile_definitions(${target} PRIVATE HAVE_OBSCONFIG_H
   $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
 target_link_libraries(${target} PRIVATE "${STUDIO_GRAPH_CORE_LIBRARY}"
   $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES> Threads::Threads ${CMAKE_DL_LIBS}
   $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
 if(MSVC)
  target_compile_options(${target} PRIVATE /utf-8)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
 endif()
endfunction()

add_graph_check(studio-active-graph-check graph-check.cpp)
add_graph_check(studio-active-scene-check scene-check.cpp)
add_graph_check(studio-active-cycle-check cycle-check.cpp)
# These mutex readers live in the executables and call no hidden core function.
add_graph_check(studio-audio-hide-group-check pcm-check.cpp hide-shim.c)
add_graph_check(studio-active-contention-check contention-check.cpp hide-shim.c)
add_graph_check(studio-active-overflow-check overflow-check.cpp)
add_graph_check(studio-active-preflight-lock-check preflight-lock-check.cpp preflight-probe.c)
add_graph_check(studio-active-transition-check transition-check.cpp transition-adapter.c
 "${STUDIO_GRAPH_TRANSITION_SOURCE_DIR}/transition-cut.c")
target_include_directories(studio-active-transition-check PRIVATE "${STUDIO_GRAPH_TRANSITION_SOURCE_DIR}")
if(STUDIO_GRAPH_BUILD_COST)
 add_graph_check(studio-active-gate-cost-check gate-cost-check.cpp)
endif()

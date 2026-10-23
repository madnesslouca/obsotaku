cmake_minimum_required(VERSION 3.28)
project(studio_output_start_failure_checks LANGUAGES C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(STUDIO_FAILURE_SDK_SOURCE_ROOT "" CACHE PATH "Matching complete external OBS SDK source")
set(STUDIO_FAILURE_CORE_LIBRARY "" CACHE FILEPATH "Exact libobs shared library or Windows import library")
set(STUDIO_FAILURE_CONFIG_INCLUDE_DIR "" CACHE PATH "Matching SDK build/config headers")
set(STUDIO_FAILURE_GENERATED_INCLUDE_DIR "" CACHE PATH "Matching SDK build/libobs headers")
set(STUDIO_FAILURE_BEFORE_SOURCE_ROOT "" CACHE PATH "Baseline root containing plugins/ source files")
set(STUDIO_FAILURE_AFTER_SOURCE_ROOT "" CACHE PATH "Candidate root containing plugins/ source files")
set(STUDIO_FAILURE_INTERMEDIATE_V1_SOURCE_ROOT "" CACHE PATH "Optional early-refusal negative control source")
set(STUDIO_FAILURE_INTERMEDIATE_V4_SOURCE_ROOT "" CACHE PATH "Optional handoff source without the self-join guard")

foreach(required STUDIO_FAILURE_SDK_SOURCE_ROOT STUDIO_FAILURE_CORE_LIBRARY
    STUDIO_FAILURE_CONFIG_INCLUDE_DIR STUDIO_FAILURE_GENERATED_INCLUDE_DIR
    STUDIO_FAILURE_BEFORE_SOURCE_ROOT STUDIO_FAILURE_AFTER_SOURCE_ROOT)
  if(NOT IS_ABSOLUTE "${${required}}" OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "Set ${required} to an absolute existing matching input")
  endif()
endforeach()
foreach(input "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/libobs/obs.h"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-ffmpeg/obs-ffmpeg-mux.h"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/flv-mux.c"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/librtmp/amf.c"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/rtmp-av1.c"
    "${STUDIO_FAILURE_CONFIG_INCLUDE_DIR}/obsconfig.h")
  if(NOT EXISTS "${input}")
    message(FATAL_ERROR "Missing required SDK input: ${input}")
  endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/cmake/finders")
find_package(libobs CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(jansson REQUIRED)
find_package(MbedTLS REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil)

set(input_manifest "{}")
function(record_input key path)
  file(SHA256 "${path}" input_hash)
  string(JSON input_manifest SET "${input_manifest}" "${key}" "\"${input_hash}\"")
  set(input_manifest "${input_manifest}" PARENT_SCOPE)
endfunction()
record_input(core_link_library "${STUDIO_FAILURE_CORE_LIBRARY}")
foreach(relative_path libobs/obs.h plugins/obs-ffmpeg/obs-ffmpeg-mux.h
    plugins/obs-ffmpeg/obs-ffmpeg-formats.h plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.h
    plugins/obs-outputs/flv-mux.c plugins/obs-outputs/flv-mux.h
    plugins/obs-outputs/librtmp/amf.c plugins/obs-outputs/librtmp/amf.h
    plugins/obs-outputs/librtmp/rtmp.h plugins/obs-outputs/rtmp-av1.c)
  record_input("sdk/${relative_path}" "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/${relative_path}")
endforeach()

function(configure_failure_check target)
  target_include_directories(${target} BEFORE PRIVATE
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/libobs"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/deps/libcaption"
    "${STUDIO_FAILURE_CONFIG_INCLUDE_DIR}" "${STUDIO_FAILURE_GENERATED_INCLUDE_DIR}"
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(${target} PRIVATE HAVE_OBSCONFIG_H
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
  target_link_libraries(${target} PRIVATE "${STUDIO_FAILURE_CORE_LIBRARY}"
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES> Threads::Threads
    $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /Gy /Gw)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_options(${target} PRIVATE /OPT:REF)
  else()
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections -O1)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
endfunction()

add_executable(studio-output-start-failure-helper pipe-helper.c)

function(add_failure_variant label source_root build_mux)
  foreach(relative_path plugins/obs-outputs/rtmp-stream.c plugins/obs-outputs/rtmp-stream.h)
    if(NOT EXISTS "${source_root}/${relative_path}")
      message(FATAL_ERROR "Missing ${label} source: ${source_root}/${relative_path}")
    endif()
    record_input("${label}/${relative_path}" "${source_root}/${relative_path}")
  endforeach()
  set(input_manifest "${input_manifest}" PARENT_SCOPE)
  set(target "studio-rtmp-start-failure-${label}")
  add_executable(${target} rtmp-check.c
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/flv-mux.c"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/librtmp/amf.c"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/rtmp-av1.c")
  configure_failure_check(${target})
  target_include_directories(${target} BEFORE PRIVATE
    "${source_root}/plugins/obs-outputs"
    "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs")
  target_compile_definitions(${target} PRIVATE USE_MBEDTLS CRYPTO
    TEST_PLUGIN_SOURCE="${source_root}/plugins/obs-outputs/rtmp-stream.c")
  target_link_libraries(${target} PRIVATE jansson::jansson MbedTLS::mbedtls
    $<$<PLATFORM_ID:Windows>:ws2_32>)

  if(build_mux)
    set(mux_source "${source_root}/plugins/obs-ffmpeg/obs-ffmpeg-mux.c")
    if(NOT EXISTS "${mux_source}")
      message(FATAL_ERROR "Missing ${label} mux source: ${mux_source}")
    endif()
    record_input("${label}/plugins/obs-ffmpeg/obs-ffmpeg-mux.c" "${mux_source}")
    set(input_manifest "${input_manifest}" PARENT_SCOPE)
    set(target "studio-mux-start-failure-${label}")
    add_executable(${target} mux-check.c)
    configure_failure_check(${target})
    target_include_directories(${target} PRIVATE
      "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-ffmpeg")
    target_compile_definitions(${target} PRIVATE TEST_PLUGIN_SOURCE="${mux_source}")
    target_link_libraries(${target} PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
  endif()
endfunction()

add_failure_variant(before "${STUDIO_FAILURE_BEFORE_SOURCE_ROOT}" TRUE)
add_failure_variant(after "${STUDIO_FAILURE_AFTER_SOURCE_ROOT}" TRUE)
if(STUDIO_FAILURE_INTERMEDIATE_V1_SOURCE_ROOT)
  add_failure_variant(intermediate-v1 "${STUDIO_FAILURE_INTERMEDIATE_V1_SOURCE_ROOT}" FALSE)
endif()
if(STUDIO_FAILURE_INTERMEDIATE_V4_SOURCE_ROOT)
  add_failure_variant(intermediate-v4 "${STUDIO_FAILURE_INTERMEDIATE_V4_SOURCE_ROOT}" FALSE)
endif()
# MSVC can retain output-info callbacks even with /Gy and /OPT:REF.
# Keep their real non-network helpers; excluded connector entry points abort.
option(STUDIO_FAILURE_VERIFY_RETAINED_LINK_SUPPORT
  "Compile retained-link support for a non-Windows diagnostic build" OFF)
if(WIN32 OR STUDIO_FAILURE_VERIFY_RETAINED_LINK_SUPPORT)
  foreach(relative_path plugins/obs-outputs/librtmp/log.c
      plugins/obs-outputs/net-if.c plugins/obs-outputs/rtmp-hevc.c)
    record_input("sdk/${relative_path}" "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/${relative_path}")
  endforeach()
  record_input(link_guards "${CMAKE_CURRENT_SOURCE_DIR}/rtmp-link-guards.c")
  foreach(label before after intermediate-v1 intermediate-v4)
    if(TARGET studio-rtmp-start-failure-${label})
      target_sources(studio-rtmp-start-failure-${label} PRIVATE
        rtmp-link-guards.c
        "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/librtmp/log.c"
        "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/net-if.c"
        "${STUDIO_FAILURE_SDK_SOURCE_ROOT}/plugins/obs-outputs/rtmp-hevc.c")
      target_link_libraries(studio-rtmp-start-failure-${label} PRIVATE
        $<$<PLATFORM_ID:Windows>:iphlpapi>)
    endif()
  endforeach()
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk-manifest.json" "${input_manifest}\n")

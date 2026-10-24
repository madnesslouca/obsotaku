cmake_minimum_required(VERSION 3.28)
project(studio_file_start_failure_checks LANGUAGES C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(STUDIO_FILE_SDK_SOURCE_ROOT "" CACHE PATH "Complete matching OBS SDK source")
set(STUDIO_FILE_CORE_LIBRARY "" CACHE FILEPATH "Exact libobs library or Windows import library")
set(STUDIO_FILE_CONFIG_INCLUDE_DIR "" CACHE PATH "Matching build/config headers")
set(STUDIO_FILE_GENERATED_INCLUDE_DIR "" CACHE PATH "Matching build/libobs headers")
set(STUDIO_FILE_BEFORE_SOURCE_ROOT "" CACHE PATH "Baseline plugin source root")
set(STUDIO_FILE_AFTER_SOURCE_ROOT "" CACHE PATH "Candidate plugin source root")
set(STUDIO_FILE_INTERMEDIATE_SOURCE_ROOT "" CACHE PATH "Optional intermediate negative control")
foreach(required STUDIO_FILE_SDK_SOURCE_ROOT STUDIO_FILE_CORE_LIBRARY
    STUDIO_FILE_CONFIG_INCLUDE_DIR STUDIO_FILE_GENERATED_INCLUDE_DIR
    STUDIO_FILE_BEFORE_SOURCE_ROOT STUDIO_FILE_AFTER_SOURCE_ROOT)
  if(NOT IS_ABSOLUTE "${${required}}" OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "Set ${required} to an absolute existing matching input")
  endif()
endforeach()
list(APPEND CMAKE_MODULE_PATH "${STUDIO_FILE_SDK_SOURCE_ROOT}/cmake/finders")
find_package(libobs CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(MbedTLS REQUIRED)

set(input_manifest "{}")
function(record_input key path)
  file(SHA256 "${path}" input_hash)
  string(JSON input_manifest SET "${input_manifest}" "${key}" "\"${input_hash}\"")
  set(input_manifest "${input_manifest}" PARENT_SCOPE)
endfunction()
record_input(core_link_library "${STUDIO_FILE_CORE_LIBRARY}")
record_input(check "${CMAKE_CURRENT_SOURCE_DIR}/check.c")
foreach(relative libobs/obs.h plugins/obs-outputs/flv-mux.c
    plugins/obs-outputs/librtmp/amf.c plugins/obs-outputs/librtmp/log.c
    plugins/obs-outputs/rtmp-av1.c plugins/obs-outputs/rtmp-hevc.c)
  record_input("sdk/${relative}" "${STUDIO_FILE_SDK_SOURCE_ROOT}/${relative}")
endforeach()
function(add_file_variant label source_root)
  foreach(relative plugins/obs-outputs/null-output.c plugins/obs-outputs/flv-output.c)
    if(NOT EXISTS "${source_root}/${relative}")
      message(FATAL_ERROR "Missing ${label} source: ${source_root}/${relative}")
    endif()
    record_input("${label}/${relative}" "${source_root}/${relative}")
  endforeach()
  set(input_manifest "${input_manifest}" PARENT_SCOPE)
  set(target "studio-file-start-failure-${label}")
  add_executable(${target} check.c
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs/flv-mux.c"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs/librtmp/amf.c"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs/librtmp/log.c"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs/rtmp-av1.c"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs/rtmp-hevc.c")
  target_include_directories(${target} BEFORE PRIVATE
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/libobs"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/deps/libcaption"
    "${STUDIO_FILE_CONFIG_INCLUDE_DIR}" "${STUDIO_FILE_GENERATED_INCLUDE_DIR}"
    "${STUDIO_FILE_SDK_SOURCE_ROOT}/plugins/obs-outputs"
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(${target} PRIVATE USE_MBEDTLS CRYPTO HAVE_OBSCONFIG_H
    TEST_NULL_SOURCE="${source_root}/plugins/obs-outputs/null-output.c"
    TEST_FLV_SOURCE="${source_root}/plugins/obs-outputs/flv-output.c"
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_COMPILE_DEFINITIONS>)
  target_link_libraries(${target} PRIVATE "${STUDIO_FILE_CORE_LIBRARY}"
    $<TARGET_PROPERTY:OBS::libobs,INTERFACE_LINK_LIBRARIES>
    Threads::Threads MbedTLS::mbedtls $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /Gy /Gw)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_options(${target} PRIVATE /OPT:REF)
  else()
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections -O1)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
endfunction()
add_file_variant(before "${STUDIO_FILE_BEFORE_SOURCE_ROOT}")
add_file_variant(after "${STUDIO_FILE_AFTER_SOURCE_ROOT}")
if(STUDIO_FILE_INTERMEDIATE_SOURCE_ROOT)
  if(NOT IS_ABSOLUTE "${STUDIO_FILE_INTERMEDIATE_SOURCE_ROOT}")
    message(FATAL_ERROR "Intermediate source root must be absolute")
  endif()
  add_file_variant(intermediate "${STUDIO_FILE_INTERMEDIATE_SOURCE_ROOT}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk-manifest.json" "${input_manifest}\n")

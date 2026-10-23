cmake_minimum_required(VERSION 3.28)
project(studio_build_dependencies NONE)
enable_testing()
add_test(
  NAME studio-build-dependencies
  COMMAND
    "${CMAKE_COMMAND}"
    "-DMODULE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/common/buildspec_common.cmake"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check.cmake"
)

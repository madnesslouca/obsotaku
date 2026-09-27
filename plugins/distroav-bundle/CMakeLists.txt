cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_BUNDLED_DISTROAV "Bundle DistroAV NDI support with OBS" ON)

if(NOT ENABLE_BUNDLED_DISTROAV)
  add_custom_target(distroav-bundle)
  target_disable(distroav-bundle)
  return()
endif()

include(FetchContent)

# Keep the binary bundle reproducible. DistroAV remains dynamically linked to
# the separately installed NDI Runtime, which is intentionally not redistributed.
FetchContent_Declare(
  distroav_release
  URL "https://github.com/DistroAV/DistroAV/releases/download/6.2.1/distroav-6.2.1-windows-x64-portable.zip"
  URL_HASH "SHA256=aefea7d559c898abaf1e2f0faeb569168ce4bb43bc0e9451d2b7dda37af308f7"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(distroav_release)

set(distroav_dll "${distroav_release_SOURCE_DIR}/obs-plugins/64bit/distroav.dll")
set(distroav_data "${distroav_release_SOURCE_DIR}/data/obs-plugins/distroav")

if(NOT EXISTS "${distroav_dll}" OR NOT IS_DIRECTORY "${distroav_data}")
  message(FATAL_ERROR "The pinned DistroAV archive does not have the expected portable layout")
endif()

add_custom_target(
  distroav-bundle
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_PLUGIN_DESTINATION}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${distroav_dll}"
          "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_PLUGIN_DESTINATION}/distroav.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-plugins/distroav"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${distroav_data}"
          "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-plugins/distroav"
  COMMENT "Bundling DistroAV 6.2.1"
  VERBATIM
)
set_target_properties(distroav-bundle PROPERTIES FOLDER plugins)

# OBS' Windows dependency bundler expects every safe-module entry to also be a
# CMake target. This bridge represents the prebuilt module and guarantees that
# its copy step runs anywhere OBS expects the normal `distroav` target.
add_custom_target(distroav DEPENDS distroav-bundle)
set_target_properties(distroav PROPERTIES FOLDER plugins)

# Include the external module in OBS' normal safe-module allowlist.
set_property(GLOBAL APPEND PROPERTY OBS_MODULES_ENABLED distroav)
set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "DistroAV 6.2.1 (NDI)")

install(FILES "${distroav_dll}" DESTINATION "${OBS_PLUGIN_DESTINATION}" COMPONENT Runtime)
install(
  DIRECTORY "${distroav_data}/"
  DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/distroav"
  COMPONENT Runtime
)
install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
  DESTINATION "${OBS_DATA_DESTINATION}/obs-studio/licenses"
  RENAME "DistroAV-NDI-NOTICE.md"
  COMPONENT Runtime
)

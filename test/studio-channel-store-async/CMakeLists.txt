cmake_minimum_required(VERSION 3.28)
project(studio_channel_store_async LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(libobs REQUIRED)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This fixture's dead-code link isolation is currently validated on Linux only")
endif()
set(FRONTEND "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend")
add_executable(studio-channel-store-async-check check.cpp
  "${FRONTEND}/utility/SecureTokenStore.cpp"
  "${FRONTEND}/utility/StreamPlatform.cpp"
  "${FRONTEND}/utility/MultistreamChannelPlan.cpp"
  "${FRONTEND}/oauth/ConnectedAccountManager.cpp")
target_compile_options(studio-channel-store-async-check PRIVATE -ffunction-sections -fdata-sections)
target_link_options(studio-channel-store-async-check PRIVATE -Wl,--gc-sections)
target_include_directories(studio-channel-store-async-check BEFORE PRIVATE stubs "${FRONTEND}")
target_link_libraries(studio-channel-store-async-check PRIVATE Qt6::Core OBS::libobs)
enable_testing()
add_test(NAME studio-channel-store-async COMMAND studio-channel-store-async-check)

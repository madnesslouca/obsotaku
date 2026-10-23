cmake_minimum_required(VERSION 3.28)
project(studio-oauth-removal LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core)
set(OBS_CONFIG_DIR "" CACHE PATH "Existing generated obsconfig.h directory; headers only")
if(NOT EXISTS "${OBS_CONFIG_DIR}/obsconfig.h")
  message(FATAL_ERROR "Set OBS_CONFIG_DIR to an existing config directory; this helper builds no libobs")
endif()
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(studio-oauth-removal-check check.cpp
  "${REPO}/frontend/oauth/ConnectedAccountManager.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-oauth-removal-check PRIVATE "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11" "${OBS_CONFIG_DIR}")
target_link_libraries(studio-oauth-removal-check PRIVATE Threads::Threads Qt6::Core)
if(MSVC)
  target_compile_options(studio-oauth-removal-check PRIVATE /utf-8)
  target_compile_definitions(studio-oauth-removal-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

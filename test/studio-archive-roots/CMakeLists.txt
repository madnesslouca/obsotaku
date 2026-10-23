cmake_minimum_required(VERSION 3.28)
project(studio_archive_roots LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(STUDIO_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Source tree containing the real Archive implementation")
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(studio-archive-roots-check check.cpp fakes.cpp
 "${STUDIO_SOURCE_ROOT}/frontend/utility/CloudBackupArchive.cpp"
 "${STUDIO_SOURCE_ROOT}/frontend/utility/CloudBackupCrypto.cpp"
 "${STUDIO_SOURCE_ROOT}/frontend/utility/StreamPlatform.cpp"
 "${STUDIO_SOURCE_ROOT}/frontend/oauth/OAuthTokenSet.cpp"
 "${STUDIO_SOURCE_ROOT}/deps/json11/json11.cpp")
target_include_directories(studio-archive-roots-check PRIVATE fake "${STUDIO_SOURCE_ROOT}/frontend" "${STUDIO_SOURCE_ROOT}/deps/json11" "${STUDIO_SOURCE_ROOT}/libobs")
target_link_libraries(studio-archive-roots-check PRIVATE Qt6::Core)
if(WIN32)
 find_package(MbedTLS CONFIG REQUIRED)
 target_compile_definitions(studio-archive-roots-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
 target_link_libraries(studio-archive-roots-check PRIVATE MbedTLS::mbedtls Crypt32)
 if(MSVC)
  target_compile_options(studio-archive-roots-check PRIVATE /utf-8)
 endif()
endif()

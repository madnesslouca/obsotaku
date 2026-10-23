cmake_minimum_required(VERSION 3.28)
project(studio-cloud-backup LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Scheduler's real method bodies, with only the enormous OBSBasic declaration
# replaced by a fixture class; this does not certify the full OBS main window.
file(READ "${REPO}/frontend/widgets/OBSBasic_CloudBackup.cpp" scheduler)
string(REPLACE "#include \"OBSBasic.hpp\"" "#include <FakeOBSBasic.hpp>" scheduler "${scheduler}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/scheduler-adapter.cpp" "${scheduler}")
add_executable(studio-cloud-backup-check check.cpp fakes.cpp fake/oauth/AuthListener.hpp
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.cpp"
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.hpp"
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/scheduler-adapter.cpp")
target_include_directories(studio-cloud-backup-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-backup-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
  target_compile_options(studio-cloud-backup-check PRIVATE /utf-8)
  target_compile_definitions(studio-cloud-backup-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Directed pre-modal/config-session regression; no scheduler/header adaptation.
add_executable(studio-cloud-modal-check modal-check.cpp fakes.cpp fake/oauth/AuthListener.hpp fake/UrlSink.hpp
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.cpp"
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.hpp"
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-cloud-modal-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-modal-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-cloud-modal-check PRIVATE /utf-8)
 target_compile_definitions(studio-cloud-modal-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(studio-cloud-format-check format-check.cpp fakes.cpp
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-cloud-format-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-format-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-cloud-format-check PRIVATE /utf-8)
 target_compile_definitions(studio-cloud-format-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_definitions(studio-cloud-format-check PRIVATE FORMAT_HELPERS)

add_executable(studio-cloud-transaction-check transaction-check.cpp fakes.cpp
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-cloud-transaction-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-transaction-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-cloud-transaction-check PRIVATE /utf-8)
 target_compile_definitions(studio-cloud-transaction-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(studio-cloud-session-check session-check.cpp fakes.cpp
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
add_executable(studio-cloud-settings-check settings-check.cpp fakes.cpp fake/oauth/AuthListener.hpp
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.cpp"
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.hpp"
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
foreach(target studio-cloud-session-check studio-cloud-settings-check)
 target_include_directories(${target} PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
 target_link_libraries(${target} PRIVATE Threads::Threads Qt6::Widgets)
 if(MSVC)
  target_compile_options(${target} PRIVATE /utf-8)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
 endif()
endforeach()

add_executable(studio-cloud-registration-check registration-check.cpp fakes.cpp
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-cloud-registration-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-registration-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-cloud-registration-check PRIVATE /utf-8)
 target_compile_definitions(studio-cloud-registration-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(studio-cloud-archive-ui-check archive-ui-check.cpp fakes.cpp fake/oauth/AuthListener.hpp fake/UrlSink.hpp
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.cpp"
  "${REPO}/frontend/dialogs/GoogleDriveBackupDialog.hpp"
  "${REPO}/frontend/oauth/GoogleDriveBackupClient.cpp"
  "${REPO}/frontend/oauth/OAuthTokenSet.cpp"
  "${REPO}/frontend/utility/SecureTokenStore.cpp"
  "${REPO}/frontend/utility/StreamPlatform.cpp"
  "${REPO}/deps/json11/json11.cpp")
target_include_directories(studio-cloud-archive-ui-check PRIVATE fake "${REPO}/frontend" "${REPO}/libobs" "${REPO}/deps/json11")
target_link_libraries(studio-cloud-archive-ui-check PRIVATE Threads::Threads Qt6::Widgets)
if(MSVC)
 target_compile_options(studio-cloud-archive-ui-check PRIVATE /utf-8)
 target_compile_definitions(studio-cloud-archive-ui-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

cmake_minimum_required(VERSION 3.28)
project(studio_modal_guards LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(libobs REQUIRED)
set(FRONTEND "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend")
add_executable(studio-modal-guards-check check.cpp
  "${FRONTEND}/dialogs/MultistreamAccountsDialog.cpp"
  "${FRONTEND}/dialogs/MultistreamAccountsDialog.hpp"
  "${FRONTEND}/oauth/AuthListener.cpp"
  "${FRONTEND}/oauth/AuthListener.hpp"
  "${FRONTEND}/utility/SecureTokenStore.cpp"
  "${FRONTEND}/utility/StreamPlatform.cpp")
target_include_directories(studio-modal-guards-check BEFORE PRIVATE stubs "${FRONTEND}")
target_link_libraries(studio-modal-guards-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs)
enable_testing()
foreach(case none profile-id profile-secret client-id client-secret endpoint-id endpoint-secret
             profile-late client-late endpoint-late cancel-id cancel-secret env-id env-secret port-id port-secret outer-client-before outer-client-after)
  add_test(NAME studio-modal-guards-${case} COMMAND studio-modal-guards-check ${case})
  set_tests_properties(studio-modal-guards-${case} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 8)
endforeach()

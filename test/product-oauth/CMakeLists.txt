if(NOT OS_WINDOWS)
  return()
endif()

set(PRODUCT_FRONTEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend")
cmake_path(NORMAL_PATH PRODUCT_FRONTEND_DIR)

add_executable(product-oauth-tests)

target_sources(
  product-oauth-tests
  PRIVATE
    OAuthPrimitivesTest.cpp
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthHttpClient.cpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthHttpClient.hpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthPkce.cpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthPkce.hpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthTokenSet.cpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/OAuthTokenSet.hpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/PlatformOAuthClient.cpp"
    "${PRODUCT_FRONTEND_DIR}/oauth/PlatformOAuthClient.hpp"
    "${PRODUCT_FRONTEND_DIR}/utility/SecureTokenStore.hpp"
    "${PRODUCT_FRONTEND_DIR}/utility/SecureTokenStore_Windows.cpp"
    "${PRODUCT_FRONTEND_DIR}/utility/StreamPlatform.cpp"
    "${PRODUCT_FRONTEND_DIR}/utility/StreamPlatform.hpp"
)

target_include_directories(product-oauth-tests PRIVATE "${PRODUCT_FRONTEND_DIR}")
target_link_libraries(product-oauth-tests PRIVATE Qt::Core CURL::libcurl OBS::json11 advapi32)

set_target_properties(product-oauth-tests PROPERTIES FOLDER tests)

# Qt and libcurl live in the dependency prefix, which is not on PATH when CTest
# launches the executable. Copy the runtime DLLs next to it instead.
add_custom_command(
  TARGET product-oauth-tests
  POST_BUILD
  COMMAND
    "$<$<BOOL:$<TARGET_RUNTIME_DLLS:product-oauth-tests>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:product-oauth-tests>;$<TARGET_FILE_DIR:product-oauth-tests>>"
  COMMAND_EXPAND_LISTS
  VERBATIM
)

add_test(NAME product-oauth-primitives COMMAND product-oauth-tests)

cmake_minimum_required(VERSION 3.28)
project(studio_clips_library_check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(libobs REQUIRED)
add_executable(studio-clips-library-check clips-library-check.cpp
  ../../frontend/docks/StudioClipsDock.cpp ../../frontend/docks/StudioClipsDock.hpp
  ../../frontend/docks/OBSDock.hpp)
target_include_directories(studio-clips-library-check BEFORE PRIVATE support ../../frontend)
target_link_libraries(studio-clips-library-check PRIVATE Qt6::Widgets OBS::libobs)
enable_testing()
add_test(NAME studio-clips-library COMMAND studio-clips-library-check)
set_tests_properties(studio-clips-library PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

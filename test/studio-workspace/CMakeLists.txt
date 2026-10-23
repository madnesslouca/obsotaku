cmake_minimum_required(VERSION 3.28)
project(studio-workspace-check LANGUAGES CXX)
enable_language(C)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
add_library(studio-monitoring-device-check MODULE monitoring-device-check.cpp)
target_link_libraries(studio-monitoring-device-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-monitoring-device-check PROPERTIES PREFIX "")
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(studio-chat-provider-errors-check chat-provider-errors-check.cpp
  ../../frontend/chat/YouTubeChatConnection.cpp ../../frontend/chat/YouTubeChatConnection.hpp
  ../../frontend/chat/ChatConnection.cpp ../../frontend/chat/ChatConnection.hpp)
set_target_properties(studio-chat-provider-errors-check PROPERTIES AUTOMOC ON)
target_include_directories(studio-chat-provider-errors-check PRIVATE ../../frontend)
target_link_libraries(studio-chat-provider-errors-check PRIVATE Qt6::Core Qt6::Network)
add_test(NAME studio-chat-provider-errors COMMAND studio-chat-provider-errors-check)
add_library(studio-workspace-check MODULE workspace-check.cpp)
target_link_libraries(studio-workspace-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-workspace-check PROPERTIES PREFIX "")
add_library(studio-clips-check MODULE clips-check.cpp)
target_link_libraries(studio-clips-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-clips-check PROPERTIES PREFIX "")
add_library(studio-features-check MODULE features-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamAudioRouting.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-features-check PRIVATE ../../frontend)
target_link_libraries(studio-features-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-features-check PROPERTIES PREFIX "")
find_package(Qt6 REQUIRED COMPONENTS Test)
add_library(studio-canvas-context-check MODULE canvas-context-check.cpp)
target_link_libraries(studio-canvas-context-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-canvas-context-check PROPERTIES PREFIX "")
add_library(studio-brain-audit-check MODULE brain-audit-check.cpp)
target_link_libraries(studio-brain-audit-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-brain-audit-check PROPERTIES PREFIX "")
add_library(studio-audio-buttons-check MODULE audio-buttons-check.cpp)
target_link_libraries(studio-audio-buttons-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-audio-buttons-check PROPERTIES PREFIX "")
add_library(studio-scene-audio-check MODULE scene-audio-check.cpp)
target_link_libraries(studio-scene-audio-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-scene-audio-check PROPERTIES PREFIX "")
add_library(studio-mixer-strip-check MODULE mixer-strip-check.cpp)
target_link_libraries(studio-mixer-strip-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-mixer-strip-check PROPERTIES PREFIX "")

# This optional target needs the same SpeexDSP library as the production plugin.
find_path(STUDIO_SPEEX_INCLUDE speex/speex_preprocess.h)
find_library(STUDIO_SPEEX_LIBRARY NAMES speexdsp)
if(STUDIO_SPEEX_INCLUDE AND STUDIO_SPEEX_LIBRARY)
  add_executable(studio-effects-dsp-check effects-dsp-check.c
    ../../plugins/obs-filters/compressor-filter.c
    ../../plugins/obs-filters/studio-audio-filters.c
    ../../plugins/obs-filters/noise-suppress-filter.c)
  target_compile_definitions(studio-effects-dsp-check PRIVATE LIBSPEEXDSP_ENABLED)
  target_include_directories(studio-effects-dsp-check PRIVATE "${STUDIO_SPEEX_INCLUDE}")
  target_link_libraries(studio-effects-dsp-check PRIVATE OBS::libobs "${STUDIO_SPEEX_LIBRARY}")
  if(UNIX)
    target_link_libraries(studio-effects-dsp-check PRIVATE m)
  endif()
endif()

add_executable(studio-vod-routing-check vod-routing-check.cpp ../../frontend/utility/StudioVodRouting.cpp)
add_library(studio-vod-controls-check MODULE vod-controls-check.cpp)
target_link_libraries(studio-vod-controls-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-vod-controls-check PROPERTIES PREFIX "")

add_library(studio-vst3-mixer-check MODULE vst3-mixer-check.cpp)
target_link_libraries(studio-vst3-mixer-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-vst3-mixer-check PROPERTIES PREFIX "")

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(studio-vst3-mixer-check PRIVATE X11::X11)
endif()

# Compiles the real dock/providers with isolated app/config/credential adapters.
# Network is replaced by QNetworkReply fixtures; no platform messages are sent.
find_package(Qt6 REQUIRED COMPONENTS Network Svg)
add_executable(studio-chat-check chat-check.cpp
  ../../frontend/chat/ChatEmotes.cpp ../../frontend/chat/ChatEmotes.hpp
  ../../frontend/chat/ChatEventSocket.cpp ../../frontend/chat/ChatEventSocket.hpp
  ../../frontend/chat/TwitchEventSub.cpp ../../frontend/chat/TwitchEventSub.hpp
  ../../frontend/chat/ChatConnection.cpp ../../frontend/chat/ChatConnection.hpp
  ../../frontend/chat/MultiStreamChatAggregator.cpp ../../frontend/chat/MultiStreamChatAggregator.hpp
  ../../frontend/chat/YouTubeChatConnection.cpp ../../frontend/chat/YouTubeChatConnection.hpp
  ../../frontend/chat/TwitchChatConnection.cpp ../../frontend/chat/TwitchChatConnection.hpp
  ../../frontend/chat/KickChatConnection.cpp ../../frontend/chat/KickChatConnection.hpp
  ../../frontend/docks/UnifiedChatDock.cpp ../../frontend/docks/UnifiedChatDock.hpp
  ../../frontend/docks/OBSDock.hpp ../../frontend/utility/StreamPlatform.cpp
  ../../frontend/utility/PlatformIconProvider.cpp ../../frontend/utility/ChatBadgeIcons.cpp)
set_target_properties(studio-chat-check PROPERTIES AUTOMOC ON)
target_include_directories(studio-chat-check BEFORE PRIVATE chat-support ../../frontend)
target_link_libraries(studio-chat-check PRIVATE Qt6::Widgets Qt6::Test Qt6::Network Qt6::Svg OBS::libobs)
target_compile_definitions(studio-chat-check PRIVATE CHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_library(studio-chat-runtime-check MODULE chat-runtime-check.cpp)
target_link_libraries(studio-chat-runtime-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-chat-runtime-check PROPERTIES PREFIX "")
enable_testing()
add_test(NAME studio-chat COMMAND studio-chat-check)
set_tests_properties(studio-chat PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# Opt-in public image GETs only. Not part of the offline CTest suite.
add_executable(studio-chat-cdn-check chat-cdn-check.cpp
  ../../frontend/chat/ChatEmotes.cpp ../../frontend/chat/ChatEmotes.hpp
  ../../frontend/chat/ChatEventSocket.cpp ../../frontend/chat/ChatEventSocket.hpp)
set_target_properties(studio-chat-cdn-check PROPERTIES AUTOMOC ON)
target_include_directories(studio-chat-cdn-check PRIVATE ../../frontend)
target_link_libraries(studio-chat-cdn-check PRIVATE Qt6::Widgets Qt6::Network)

# Opt-in real RTMP failure/recovery probe; no account store or public services.
add_library(studio-multistream-reconnect-check MODULE multistream-reconnect-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-multistream-reconnect-check PRIVATE ../../frontend)
target_link_libraries(studio-multistream-reconnect-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-multistream-reconnect-check PROPERTIES PREFIX "")

add_library(studio-canvas-settings-check MODULE canvas-settings-check.cpp)
target_link_libraries(studio-canvas-settings-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-canvas-settings-check PROPERTIES PREFIX "")

add_library(studio-canvas-outputs-check MODULE canvas-outputs-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-canvas-outputs-check PRIVATE ../../frontend)
target_link_libraries(studio-canvas-outputs-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-canvas-outputs-check PROPERTIES PREFIX "")

add_library(studio-dual-multistream-check MODULE dual-multistream-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-dual-multistream-check PRIVATE ../../frontend)
target_link_libraries(studio-dual-multistream-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-dual-multistream-check PROPERTIES PREFIX "")

add_library(studio-simultaneous-outputs-check MODULE simultaneous-outputs-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-simultaneous-outputs-check PRIVATE ../../frontend)
target_link_libraries(studio-simultaneous-outputs-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-simultaneous-outputs-check PROPERTIES PREFIX "")

add_library(studio-portrait-gestures-check MODULE portrait-gestures-check.cpp)
target_link_libraries(studio-portrait-gestures-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-portrait-gestures-check PROPERTIES PREFIX "")

add_library(studio-portrait-commands-check MODULE portrait-commands-check.cpp)
target_link_libraries(studio-portrait-commands-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-portrait-commands-check PROPERTIES PREFIX "")

add_library(studio-portrait-collective-check MODULE portrait-collective-check.cpp)
target_link_libraries(studio-portrait-collective-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-portrait-collective-check PROPERTIES PREFIX "")

add_executable(studio-portrait-transform-geometry-check portrait-transform-geometry-check.cpp
  ../../frontend/utility/PortraitTransformGeometry.cpp)
target_include_directories(studio-portrait-transform-geometry-check PRIVATE ../../frontend)
target_link_libraries(studio-portrait-transform-geometry-check PRIVATE Qt6::Core OBS::libobs)
add_test(NAME studio-portrait-transform-geometry COMMAND studio-portrait-transform-geometry-check)

# Opt-in native group-child gestures and independent Portrait transitions.
foreach(check IN ITEMS groups transitions imports rotation)
  add_library(studio-portrait-${check}-check MODULE portrait-${check}-check.cpp)
  target_link_libraries(studio-portrait-${check}-check PRIVATE Qt6::Widgets Qt6::Test OBS::libobs OBS::obs-frontend-api)
  set_target_properties(studio-portrait-${check}-check PROPERTIES PREFIX "")
endforeach()

# Opt-in two replay buffers alongside recording + loopback multistream.
add_library(studio-dual-replay-check MODULE dual-replay-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-dual-replay-check PRIVATE ../../frontend)
target_link_libraries(studio-dual-replay-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-dual-replay-check PROPERTIES PREFIX "")

# Pure data contracts: immutable effective defaults and nested settings copies.
add_executable(studio-output-presets-check output-presets-check.cpp
  ../../frontend/utility/StudioOutputPresetData.cpp)
target_include_directories(studio-output-presets-check PRIVATE ../../frontend)
target_link_libraries(studio-output-presets-check PRIVATE Qt6::Core OBS::libobs)
add_test(NAME studio-output-presets COMMAND studio-output-presets-check)

# Real replay admission/purge, synthetic ref-counted packets; no user session.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(REPLAY_AV QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
  if(REPLAY_AV_FOUND)
    add_executable(studio-replay-payload-check replay-payload-check.c)
    target_link_libraries(studio-replay-payload-check PRIVATE OBS::libobs PkgConfig::REPLAY_AV)
    add_test(NAME studio-replay-payload COMMAND studio-replay-payload-check)
  endif()
endif()

# Opt-in Settings-only preservation of valid externally configured resolutions.
add_library(studio-preset-settings-check MODULE preset-settings-check.cpp)
target_link_libraries(studio-preset-settings-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-preset-settings-check PROPERTIES PREFIX "")

# Opt-in actual CUE endpoint captures and independent local live/VOD/record audio.
add_library(studio-audio-cue-vod-check MODULE audio-cue-vod-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-audio-cue-vod-check PRIVATE ../../frontend)
target_link_libraries(studio-audio-cue-vod-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-audio-cue-vod-check PROPERTIES PREFIX "")

# Native Undo/Redo field isolation and frozen effective RTMP VOD archive mix.
add_library(studio-audio-vod-active-check MODULE audio-vod-active-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-audio-vod-active-check PRIVATE ../../frontend)
target_link_libraries(studio-audio-vod-active-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-audio-vod-active-check PROPERTIES PREFIX "")

# Separate Windows WASAPI variant; builds elsewhere but refuses native execution.
add_library(studio-audio-cue-vod-windows-check MODULE audio-cue-vod-windows-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-audio-cue-vod-windows-check PRIVATE ../../frontend)
target_link_libraries(studio-audio-cue-vod-windows-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-audio-cue-vod-windows-check PROPERTIES PREFIX "")

# Portable native profile regression; observes defaults before any Settings UI.
add_library(studio-profile-defaults-check MODULE profile-defaults-check.cpp)
target_link_libraries(studio-profile-defaults-check PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-profile-defaults-check PROPERTIES PREFIX "")

# Separate Windows-target active archive/Undo regression; no hardware capture.
add_library(studio-audio-vod-active-windows-check MODULE audio-vod-active-windows-check.cpp
  ../../frontend/utility/MultiStreamManager.cpp
  ../../frontend/utility/MultistreamChannelPlan.cpp
  ../../frontend/utility/StreamPlatform.cpp)
target_include_directories(studio-audio-vod-active-windows-check PRIVATE ../../frontend)
target_link_libraries(studio-audio-vod-active-windows-check PRIVATE Qt6::Widgets Qt6::Network OBS::libobs OBS::obs-frontend-api)
set_target_properties(studio-audio-vod-active-windows-check PROPERTIES PREFIX "")

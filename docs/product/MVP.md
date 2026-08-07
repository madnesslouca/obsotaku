# Simplified multistream OBS fork

This branch is the starting point for a creator-focused streaming application built on OBS Studio 32.2.1.
The product name and visual identity are intentionally undecided.

## Product goal

Let a new creator add sources, select two or more destinations, and go live without understanding OBS output
terminology. Advanced OBS controls remain available, but are not part of the default workflow.

## First release

- Windows x64 first.
- Camera, display, window, game, image, media, browser, and text sources.
- Scenes, audio mixer, local recording, and virtual camera inherited from OBS.
- Custom RTMP destinations with a per-channel enabled switch and connection status.
- Connected YouTube, Twitch, and Kick accounts with automatic ingest credentials.
- Facebook Live, TikTok, X, and Trovo destinations through a pasted stream key, because none of them exposes a
  public ingest API. The server is pre-filled from the platform catalog and the key is stored in the credential
  vault, so they behave like any other destination once configured.
- One shared video/audio encode when channel requirements are compatible.
- Automatic encoder selection and 720p/1080p quality presets.
- Preflight checks for upload capacity, missing stream keys, and unsupported settings.
- A single **Go live** action and a clear partial-failure state.

Aggregated chat, cloud relay, vertical output, and subscriptions are outside the first release.

## Architecture decision

The application is an OBS Studio fork rather than an OBS plugin. `libobs` and the existing source, scene, audio,
encoder, recording, and output implementations remain the media engine. The existing Qt frontend will be reshaped
incrementally so upstream fixes can still be merged.

`frontend/utility/MultiStreamManager` is the initial local-fan-out component. It creates one RTMP service/output per
enabled channel and attaches the encoders supplied by the normal output handler. Stream keys are only held in memory
by this component and must never be written to logs. OAuth tokens and resolved credentials are persisted only through
the operating-system credential store.

## Milestones

1. Build the unmodified 32.2.1 base and keep the normal OBS streaming path working.
2. Add and test local RTMP fan-out with shared encoders.
3. Add YouTube, Twitch, and Kick OAuth clients with secrets stored in Windows Credential Manager.
4. Resolve ingest servers and stream keys through each platform's documented API.
5. Add the channel-card UI and wire it to the output handler.
6. Replace first-run setup with a short creator-oriented wizard.
7. Add automated smoke tests, packaging, GPL notices, and source distribution.

## Current status

- OBS Studio 32.2.1 source and submodules are pinned on `product/multistream-mvp`.
- `MultiStreamManager` is wired to the channel bar and to the accounts dialog. Audio encoders are handed over
  indexed by OBS audio track, so the track selected per destination is the track that is sent.
- The platform catalog defines the OAuth flow, minimum scopes, and ingest capabilities for YouTube, Twitch, and Kick.
- The Windows Credential Manager token store is compiled into the frontend; tokens are not persisted in OBS `.ini`
  files by the new integration.
- The provider layer implements YouTube/Kick PKCE, Twitch Device Code authorization, token refresh/validation,
  account identity lookup, and API-based ingest resolution.
- `ConnectedAccountManager` securely reconnects a saved account and supplies its resolved server/key to the
  multistream channel model.
- **Tools > Streaming Accounts** now shows YouTube, Twitch, and Kick cards. Browser callbacks and Twitch Device Code
  polling run without blocking the main window, while normal configuration stores only account ids/display names.
- A persistent top channel bar mirrors the useful PRISM channel-strip behavior with platform color, account name,
  offline/connecting/live/failed state, and a per-destination toggle.
- Additional RTMP destinations reuse the active OBS video/audio encoders. A destination can be stopped or restarted
  while the other live outputs continue, and all additional outputs stop with the main stream.
- Missing developer registrations appear as an unavailable integration card rather than exposing environment
  variable instructions to end users. Public client IDs can be embedded through product CMake configuration.
- All account state lives in one configuration section per platform, `MultistreamAccount.<platform-id>`, derived
  from `StreamPlatformConfigSection()`. Nothing may invent its own section.
- `MultistreamChannelStore` owns the destination list: N channels of any platform, metadata in the configuration
  and stream keys in the credential vault. It migrates the old one-account-per-platform layout on first run.
- The channel bar mirrors the PRISM channel strip: a card per destination with the platform accent, state and
  per-destination switch, a `+` that opens the platform picker, and a collapse control. Cards show the channel's
  own profile picture when the platform returns one, and the platform logo otherwise.
- Platform logos live in `frontend/data/images/platforms/` and come from Simple Icons (CC0). `PlatformIconProvider`
  tints them with the catalog brand color and falls back to a drawn initial when a platform has no file, so a
  missing icon is never a broken card. See that directory's README for the per-platform brand guidelines.
- Each platform publishes its ingest limits in the catalog. `MultistreamPreflight` compares them against the
  active output and shows the mismatch in the bar before going live: wrong orientation, resolution that will be
  downscaled, framerate or bitrate above what the destination accepts.
- The bar polls `MultiStreamManager::Snapshot()` once a second while anything is live and shows delivery health
  per destination — send-buffer congestion, dropped frames, uptime and reconnect count — plus a running
  "N live of M" summary. A single destination can be reconnected from its card menu without touching the others.
- Product OAuth tests pass for PKCE, authorization request construction, provider policy, the account
  configuration section, and secure credential storage. They are built with `-DENABLE_PRODUCT_TESTS=ON` and run
  through CTest. Real developer registrations and deployment of the Kick token proxy are still required.
- The unified chat dock reads Twitch over IRC, Kick over a masked RFC 6455 WebSocket, and YouTube over the
  authenticated live-chat API at the polling interval the API asks for. It disconnects while hidden.
- The Windows x64 `RelWithDebInfo` build succeeds with MSVC 19.44 and Windows SDK 10.0.26100.
- The generated executable passes the `--version` process smoke test with exit code 0.

## Two-PC setup and the VOD audio track

A common requirement is a VOD without music while the live mix has it. Twitch is the only supported platform that
accepts a second audio track for the recorded VOD, so the catalog gates it with `supportsVodTrack` and the account
dialog only offers the control there. Everywhere else the archive is the live audio, and the way to publish a
different mix is to record locally with two tracks and upload that file.

When the encoding runs on a second PC, the separation has to survive the trip: whatever mixes on the gaming PC
arrives already merged. Music playing on the streaming PC removes the problem entirely. When it has to play on the
gaming PC, that machine needs to send two streams — for example an NDI program output plus a dedicated NDI filter
on the music source, which captures the source audio before the mixer, so the music can be set to
monitor-only and stay out of the program. The streaming PC then routes the program to tracks 1 and 2 and the music
to track 1 alone, and the Twitch destination uses track 1 live with track 2 as the VOD track.

## Local development build

This workstation uses the ignored `CMakeUserPresets.json` preset `product-windows-x64`, which selects Visual Studio
2022 while retaining the upstream OBS dependency metadata.

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --preset product-windows-x64
& 'C:\Program Files\CMake\bin\cmake.exe' --build --preset product-windows-x64 --target obs-studio
```

The development executable is generated at
`build_product_x64/rundir/RelWithDebInfo/bin/64bit/obs64.exe`.

# Windows conservative combined outputs follow-up

This opt-in probe derives phase0 of the dual-replay native contract. It runs
only against an explicitly identified private portable configuration and two
127.0.0.1 MediaMTX receivers. No account/key store or user host configuration is
read or modified. The previous private VM configuration is moved to a new
backup and restored after the probe is removed. Fixtures and failed attempts
are preserved separately.

Gesture/backend contract: native Settings selects Multi, recording Both,
replay Both, independent bitrates, then closes/reopens to inspect persistence.
Native Replay/Go Live/Record start six actual outputs. Main is640x36015fps,
Portrait remains the product's1080x1920 canvas at15fps. Real active encoders,
settings, audio presence, bytes and per-buffer budgets are inspected; selecting
Apply alone does not approve outputs. Save Both must create distinct clips,
canvas/pair metadata and library entries while stream/record remain active.
Pause affects recording only. Dropping only the Portrait receiver must leave
Main delivering bytes; restarting it must recover Portrait. Save followed by
normal Stop must preserve accepted pending saves. File outputs then stop while
stream continues, followed by normal stream stop and restoration of owned
sources/profile. Failed deadlines remain failures.

Acceptance also requires independent full decoding of every recording/clip and
RTMP segment, correct dimensions/rate, nonblack moving video and997Hz audio,
plus normal process close. Backend assertions alone are insufficient. This is
a virtual HDA/WARP/software-x264 short test, without physical capture,60fps,
public ingest, high-DPI gestures, third-party VST3 or long-duration stability.
The initial run uses the prior accepted executable; later source changes need
an explicitly identified new build and separate results.

# Shared encoder stop regression

Build this opt-in headless fixture against the exact baseline or candidate private adapter core
using `-DSTUDIO_TEST_CORE=/absolute/path/to/libobs`. The private core must
compile `adapter.c` internally to expose the three hidden legacy functions;
never add these exports to the installed product. This test reads the private
encoder layout: its headers must match that core exactly. The fixture registers its
own one-frame buffered encoder and a real 64×64 I420 video output. Two actual
packet callbacks share the encoder. It stops one callback, stops an absent tuple,
starts a duplicate, and finally disconnects/restarts the last callback.

The remaining callback must receive the original composition timestamp for each
buffered packet. Nonfinal and absent stops must preserve pending timing. Final
stop clears only after disconnect has drained the producer. All tests use private
objects, no application settings or peripherals. The codec payload is synthetic;
this certifies dispatch/timing ownership, not decodable video, RTMP, GPU or 60 fps.

# Existing build dependency detection

This CMake-only regression includes the actual dependency discovery helper. It
creates version markers and a Qt configuration marker in a fresh fixture,
without downloading dependencies or configuring the application.

```sh
cmake -S test/studio-build-dependencies -B /tmp/studio-build-dependencies
ctest --test-dir /tmp/studio-build-dependencies --output-on-failure
```

Nine criteria cover equal/newer/older/missing versions, skipping a non-Qt prefix,
repeated calls and propagation of the reordered prefix list. Fixture directories
use a unique name per run; results stay in the build directory. Passing this test
does not certify a dependency package, compiler or application build.

# Product release checklist

Use this checklist for every customized OBS release so fixes never remain only
on one workstation.

1. Build `obs-studio` in `RelWithDebInfo` or `Release`.
2. Run `ctest --test-dir build_product_x64 -C RelWithDebInfo -R '^product-' --output-on-failure`.
3. Start OBS once and verify horizontal-only, vertical-only, mixed horizontal +
   vertical, chat, stream information, and NDI adapter selection.
4. Confirm a clean shutdown and check the newest OBS log for multistream errors.
5. Commit the source changes and push `product/multistream-mvp` to the
   `obsotaku` remote before distributing a binary.
6. Tag the commit used for the installer or portable archive and retain its
   checksum with the release artifact.

Never treat a binary in `build_product_x64` as the only copy of a fix: build
directories are disposable and are not a source backup.

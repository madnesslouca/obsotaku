# Slideshow child ownership and snapshot lifetime

Compile the actual v1/v2 slideshow sources with empty synthetic transition
backends against an exact private adapter core. Add `adapter.c` only to that
private core; never install the fixture exports. The serial fixture checks
balanced child removal and retirement across six transition replacements per
version. Image preload/memory helpers abort if reached; no image/effect rendering
or decodable media is certified.

For the deterministic lifetime fixture, run `prepare-lifetime.py SOURCE FRESH_DEST
--variant before|after`. Originals are untouched. Build with
`STUDIO_SLIDESHOW_LIFETIME_SOURCE_DIR=FRESH_DEST` and invoke
`studio-slideshow-lifetime-check RESULT.tsv before|after`. A reader pauses
between raw load and strong-reference acquisition; the writer replaces the
transition. Weak expiration detects the original lifetime violation and prevents
an unsafe dereference. The corrected reader owns its snapshot before replacement
can retire it. Enum reentry and empty Next/Previous are included. The schedules
release the blocked reader at the appropriate ownership boundary; they do not
wait for a crash. Runtime use requires the matching private DLL/shared library.

# Compact build dependency information

This local test build statically links Qt 5, SDL2 and the emulator's selected
support libraries. It uses a recording-oriented FFmpeg build instead of the
large decoder/filter collection in the shared developer runtime. The build
recipe and dependency versions are retained with the source/build records.

Full runtime/toolchain notices are aggregated under `licenses`, with their
original relative file names. The collection deliberately includes some unused
package notices; it is not a list of every library linked into this binary.
The PE imports in `RUNTIME-DEPENDENCIES.txt` are Windows system components only.

The public beta release provides the matching mGBA source, complete applicable
dependency source packages and build/relink materials as separate assets on
the same release page. See `WIRELESS-LICENSES.md` for source access and the GPL
version 3 terms for the combined executable; source-file terms are retained.
Notices alone are not a replacement for the accompanying source materials.

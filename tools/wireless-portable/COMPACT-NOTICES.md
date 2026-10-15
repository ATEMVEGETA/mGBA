# Compact build dependency information

This local test build statically links Qt 5, SDL2 and the emulator's selected
support libraries. It uses a recording-oriented FFmpeg build instead of the
large decoder/filter collection in the shared developer runtime. The build
recipe and dependency versions are retained with the source/build records.

Full runtime/toolchain notices are aggregated under `licenses`, with their
original relative file names. The collection deliberately includes some unused
package notices; it is not a list of every library linked into this binary.
The PE imports in `RUNTIME-DEPENDENCIES.txt` are Windows system components only.

This candidate is for local testing. Before public distribution, retain the
matching source, applicable dependency source and build information, and
materials required for relinking/rebuilding statically linked libraries.
The presence of notices does not by itself establish compliance with every
distribution term. No public release is authorized by this packaging step.

# Runtime dependency notices

This portable test package contains MinGW-w64/MSYS2 runtime libraries and Qt 5
plugins, in addition to mGBA. Their license notices are collected under
`licenses/runtime` and `licenses/common`. The runtime notice collection may
also contain notices for toolchain packages not used by this executable.

The PE import closure is recorded in `RUNTIME-DEPENDENCIES.txt`; package file
hashes are recorded in `SHA256SUMS.txt`. Do not mix these DLLs and plugins with
another package. The mGBA license is included as `LICENSE.txt`; its source
notices remain in the matching source archive.

The import check does not cover every optional dynamically loaded backend or
driver. A clean-machine Qt/audio/OpenGL test is still necessary before claiming
general Windows compatibility. No Windows system DLLs or GPU drivers are copied.

This is a local, unpublished test package. A public release must retain the
matching source, applicable notices and dependency/source information. The
presence of notice files is not a legal assessment of every distribution term.

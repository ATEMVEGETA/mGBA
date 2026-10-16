# Building the wireless-only test fork

This candidate starts from official mGBA `master` commit
`143adf5aa2a60c6036810b0cd72445e13d74b0e6`. It is an unofficial development
build, not the official 0.10.5 release. The port changes only RFU support,
its Qt adapter-selection integration, tests, and documentation/packaging.
Original controls, fast-forward, BIOS and e-Reader source remain upstream.

Follow the upstream README for dependencies. The current Windows test uses
MSYS2 UCRT64 GCC, CMake/Ninja, Qt 5, SDL2, and the available optional libraries.
From a matching source checkout with those tools on PATH:

```powershell
cmake -S . -B build-wireless -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORCE_QT_VERSION=5 -DBUILD_STATIC=ON -DBUILD_SHARED=OFF -DBUILD_QT=ON -DBUILD_SDL=ON
cmake --build build-wireless --target mgba mgba-qt -j4
./tests/rfu-retry/run.ps1
./tests/rfu-retry/run.ps1 -NativeMutex
./tests/rfu-retry/run-cooperative-native.ps1
./tests/qt-wireless/run.ps1
```

Run test variants sequentially. The deterministic suites cover 160 cases,
including four-adapter discovery, pair and group regression coverage;
the Windows concurrent-owner suite covers 64; the actual Qt configuration,
menu, session-selection and four-core attachment smoke covers 194 checks
without game assets.
Read each test README for prerequisites and coverage limits. These checks do
not replace gameplay testing of the exact packaged binary.

## Portable Windows test package

Commit source changes, rebuild after that commit so the executable identifies
it, and create a matching tracked-source archive. For example:

```powershell
git archive --format=zip --output=build-wireless/source.zip HEAD
./tools/package-wireless-win.ps1 -RuntimePrefix C:/msys64/ucrt64 -SourceArchive ./build-wireless/source.zip
```

The helper requires clean source and a matching executable version. It refuses
to overwrite output, resolves PE imports recursively against the supplied
runtime, copies Qt 5 plugins and license notices, and records package file
hashes, source commit and source-archive hash. Default output is beneath the
ignored `build-wireless/dist` folder. It includes a fresh portable marker,
not a copy of personal emulator settings, logs, ROMs, BIOS files or saves.

PE imports do not describe every dynamically loaded graphics/audio/backend
dependency. A clean-machine runtime test is necessary before general Windows
compatibility claims. This is not a cross-platform packaging tool.

Nothing in these commands creates a GitHub repository or uploads a release.
Publication remains conditional on the user's manual test, matched-source
verification, review of provenance/applicable dependency notices, and explicit
approval to publish. See [the setup guide](WIRELESS-ADAPTER.md).

## Compact Windows build

The compact candidate links the UI and support libraries statically. It does
not copy DLLs from the developer runtime. Keep the larger developer package
as a fallback; build this version in a separate directory.

Prerequisites: the matching MSYS2 UCRT64 compiler and static dependency
archives, plus these isolated prefixes under the ignored `build-compact-deps`:

- `ucrt64/qt5-static`: MSYS2 Qt 5.15.19-1 static package, downloaded from
  `https://mirror.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-qt5-static-5.15.19-1-any.pkg.tar.zst`.
  Verify SHA-256 `8bbbb97279f7a7b4c1abaadad10e16d0207b8bb43733e2d3fa0f290b64a4156c`
  before extracting into `build-compact-deps`; do not replace the installed Qt.
- `ffmpeg-static`: run `tools/build-wireless-ffmpeg.sh` with MSYS2 bash. Its
  pinned sources and checksums reproduce mGBA's official recording-oriented
  encoder/muxer/filter selection without the shared runtime's unrelated
  decoder, speech recognition and shader dependencies.
- `epoxy-static`: build the official mGBA dependency fork of libepoxy at
  `8b7513e88c4f23b6b8d6757b306e431af79df1ff` with **build-time**
  `EPOXY_STATIC_LIB` enabled. Run `tools/build-wireless-epoxy.sh` with MSYS2
  bash. This fork supplies a PE TLS callback so OpenGL dispatch storage is
  initialized for the executable and each graphics thread. The ordinary
  MSYS2 static archive relies on DLL entry-point initialization and crashes
  during native OpenGL startup when used directly in an executable. Defining
  the macro only while compiling mGBA cannot repair that prebuilt archive.
- `qt-third-party-notices`: retain the exact Qt bundled library notices and
  their source-version provenance alongside the Qt package's own licenses.
  Run `tools/collect-wireless-qt-notices.ps1` to collect these pinned notices.
- `libelf-COPYING.LIB` and `libelf-COPYING.LIB.provenance.json`: retain the LGPL
  notice for the installed libelf 0.8.13 separately (MSYS2 has no libelf notice
  folder). The pinned source is `mgba-emu/dependencies` commit
  `dad3d8a2bbc452b58139ebf21bc2ab9d81b9c4e9`, path
  `libraries/libelf/COPYING.LIB`; SHA-256 is
  `7a4436f9ec37603356791c87de3bc444989befd2682d29efb3d97604e04c1852`.

Then, from a clean committed checkout:

```powershell
./tools/build-wireless-compact.ps1
./tests/qt-wireless/run.ps1 -BuildDirectory ./build-compact-static -Compiler C:/msys64/ucrt64/bin/c++.exe
./tests/qt-startup/run.ps1 -BuildDirectory ./build-compact-static -Compiler C:/msys64/ucrt64/bin/c++.exe
git archive --format=zip --output=build-compact-static/source.zip HEAD
./tools/package-wireless-compact.ps1 -RuntimePrefix C:/msys64/ucrt64 -StaticQtPrefix ./build-compact-deps/ucrt64/qt5-static -FFmpegSourceDirectory ./build-compact-deps/FFmpeg-239f2c733de417201d7ad3b3b8b0d9b63285b2b1 -SourceArchive ./build-compact-static/source.zip
```

This recipe requires CMake 3.19 or later. It preserves Qt's imported static
dependencies, selects `.a` rather than DLL import archives, and includes the
SDL2/font-rendering static dependency closure. The packager rejects any
non-Windows PE import, records exact source/binary checksums, and retains the
normal shaders, scripts, game database and license material. Run all RFU
tests above as well, plus isolated Windows-only-PATH version/menu checks and
the real production entry-point/native OpenGL startup test. The offscreen
menu suite alone does not cover default graphics initialization.

An independent checkout may reuse already-built matching dependencies
read-only by passing `-DependencyDirectory <path-to-build-compact-deps>` to
both compact helpers and a separate `-BuildDirectory`. Also pass the matching
`-StaticQtPrefix`, `-StaticEpoxyPrefix` and `-FFmpegSourceDirectory` to the
packager. This does not modify the earlier checkout or its release artifacts.
For this four-player local candidate the user performs the gameplay/startup
test; only protocol, native-mutex/stress and inert offscreen integration
fixtures were run during preparation.

Statically linked GPL/LGPL and other dependencies require matching source,
build and relink materials before publication. Preserve dependency sources,
build metadata and objects; the runtime ZIP alone is not the source bundle.
The v0.1.0-beta.1 release provides complete dependency-source and build/relink
archives alongside its exact mGBA source ZIP. See [source and licensing
information](WIRELESS-LICENSES.md). Regenerate and verify those materials for
any later executable; do not reuse a preceding version's source manifest.

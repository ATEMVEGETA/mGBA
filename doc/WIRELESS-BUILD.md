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

Run test variants sequentially. The deterministic suites cover 151 cases;
the Windows concurrent-owner suite covers 64; the actual Qt configuration,
menu and session-selection smoke covers 41 checks without game assets.
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

# mGBA with Wireless Adapter support

Unofficial Windows x64 test build based on official mGBA development source.
The interface and runtime libraries are bundled into the executables; no
separate DLLs or Qt plugin folders are required. Keep the included data,
scripts, shaders, documentation and license notices together.

To enable wireless, close all games and choose **File → GBA link type (next
session) → Wireless Adapter**. Open the first game, then use **File → New
multiplayer window** for the second game in the same application process.
Choose **Link Cable** instead to disable the Wireless Adapter.

Back up your saves before testing. This package has no ROMs, BIOS or saves.
See [the full guide](docs/WIRELESS-ADAPTER.md) for details and limitations.
Its exact source commit and checksums are recorded under `docs`.

This is the wireless-only port: original mGBA controls, fast-forward and
e-Reader behavior are retained. Source, dependency sources, build/relink
materials and release checksums are available at:

https://github.com/ATEMVEGETA/mgba/releases/tag/wireless-adapter-v0.1.0-beta.1

This compact executable statically includes GPL/LGPL libraries and is
distributed as a combined work under GNU GPL version 3. Original MPL source
terms and all dependency notices are retained. See `docs/WIRELESS-LICENSES.md`
and `docs/licenses/GPL-3.0.txt`. You may modify, rebuild and relink it.

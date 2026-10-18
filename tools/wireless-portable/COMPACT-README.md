# mGBA with Wireless Adapter support

Unofficial Windows x64 portable release v0.3.0-beta.1, based on mGBA development source.
The interface and runtime libraries are bundled into the executables; no
separate DLLs or Qt plugin folders are required. Keep the included data,
scripts, shaders, documentation and license notices together.

To enable wireless, close all games and choose **File → GBA link type (next
session) → Wireless Adapter**. Open the first game, then use **File → New
multiplayer window** for each other game in the same application process,
starting with a small population, then increasing. The safety ceiling is 40
windows, not a claim of 40-player compatibility. Independent mGBA processes
and other PCs are not connected; there is no LAN transport in this build.
Choose **Link Cable** instead to disable the Wireless Adapter.

Back up your saves before testing. This package has no ROMs, BIOS or saves.
See [the expanded-room guide](docs/WIRELESS-LARGE-ROOM.md) for details and limitations.
Its exact source commit and checksums are recorded under `docs`.

The user reported that the expanded-room candidate works perfectly and approved
this release. The exact tested population was not recorded; this is not
confirmation of 40-player gameplay or universal game compatibility. Its exact
executable source and archive hash are recorded in `docs/BUILD-INFO.txt`.

Original mGBA controls, fast-forward and e-Reader behavior are retained.
Matching application source, unchanged dependency sources and fresh build/relink
materials are separate release downloads:

https://github.com/ATEMVEGETA/mGBA/releases/tag/wireless-adapter-v0.3.0-beta.1

This compact executable statically includes GPL/LGPL libraries and is
distributed as a combined work under GNU GPL version 3. Original MPL source
terms and all dependency notices are retained. See `docs/WIRELESS-LICENSES.md`
and `docs/licenses/GPL-3.0.txt`. You may modify, rebuild and relink it.

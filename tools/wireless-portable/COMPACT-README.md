# mGBA with Wireless Adapter support

**Unpublished expanded-room prototype:** this package permits up to 40 local
wireless windows. Start testing with five, then eight; 40-player gameplay is
not established. Use **File → New multiplayer window** within one process.
Each game is independent but shares the virtual radio environment. Wireless
data groups still have a host plus at most four clients; cable stays at four.

Read [the prototype guide](docs/WIRELESS-LARGE-ROOM.md) first. The published
four-player release and VEGETA's working custom build remain unchanged. The
release links below refer to that earlier base and its dependency sources,
not a public download of this prototype. Exact prototype source/hash details
are in `docs/BUILD-INFO.txt` and its accompanying source archive.

Unofficial Windows x64 test build based on official mGBA development source.
The interface and runtime libraries are bundled into the executables; no
separate DLLs or Qt plugin folders are required. Keep the included data,
scripts, shaders, documentation and license notices together.

To enable wireless, close all games and choose **File → GBA link type (next
session) → Wireless Adapter**. Open the first game, then use **File → New
multiplayer window** for each other game in the same application process,
starting with five games, then eight. The prototype's safety ceiling is 40
windows, not a claim of 40-player compatibility. Independent mGBA processes
and other PCs are not connected; there is no LAN transport in this build.
Choose **Link Cable** instead to disable the Wireless Adapter.

Back up your saves before testing. This package has no ROMs, BIOS or saves.
See [the prototype guide](docs/WIRELESS-LARGE-ROOM.md) for details and limitations.
Its exact source commit and checksums are recorded under `docs`.

The preceding published v0.2.0-beta.1 supports four local players and received
a successful four-player Pokémon Union Room report. This expanded prototype
has different executables and has not yet received a gameplay test. Its exact
source commit and archive hash are recorded in `docs/BUILD-INFO.txt`.

Original mGBA controls, fast-forward and e-Reader behavior are retained.
The matching prototype application source accompanies this local handoff.
The preceding release's application source, unchanged dependency sources and
build/relink materials are available at the following page; it does not
contain these prototype executables or their exact application source:

https://github.com/ATEMVEGETA/mGBA/releases/tag/wireless-adapter-v0.2.0-beta.1

This compact executable statically includes GPL/LGPL libraries and is
distributed as a combined work under GNU GPL version 3. Original MPL source
terms and all dependency notices are retained. See `docs/WIRELESS-LICENSES.md`
and `docs/licenses/GPL-3.0.txt`. You may modify, rebuild and relink it.

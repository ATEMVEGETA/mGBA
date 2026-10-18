# Wireless Adapter release: source and licenses

This unofficial fork retains mGBA's copyrights and source-file license notices.
The project is not endorsed by mGBA, Nintendo or The Pokémon Company. It does
not include games, ROMs, BIOS dumps or save files.

## Compact Windows executable

The compact build statically links Qt 5 and a GPL-enabled recording-oriented
FFmpeg build, including x264/x265. The combined executable is distributed under
**GNU GPL version 3**; it is not an MPL-only or proprietary executable. The
GPL text is provided in [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt), and original
dependency notices accompany the runtime package.

MPL-covered source, including this fork's changes, remains available under MPL
2.0. For this GPL larger work it is additionally distributed under GPL version
3 pursuant to MPL 2.0 section 3.3. Original notices are not removed or replaced;
third-party files keep their own applicable terms. See Mozilla's
[MPL/GPL combination guidance](https://www.mozilla.org/en-US/MPL/2.0/combining-mpl-and-gpl/).

Recipients may study, modify, rebuild and relink the software, replace its
libraries and run their modified executable. No signature, account, activation
or private key is required. The software is supplied without warranty under
the applicable licenses. There is no added restriction on reverse engineering
or modification.

## Matching release materials

The [v0.3.0-beta.1 expanded-room release](https://github.com/ATEMVEGETA/mGBA/releases/tag/wireless-adapter-v0.3.0-beta.1)
provides the exact application source at
`cee6c249acd626c5b188bc91d042b345f6da0e5a`, its matching fresh build/relink kit,
unchanged complete dependency sources and SHA-256 checksums. Its executable
version is `0.11-codex/wireless-large-room-9168-wireless-adapter-v0.2.0-beta.1-6-gcee6c249a`.
The v0.3.0-beta.1 tag resolves to that exact executable source revision;
the embedded development description retains the older tag from build time.
Publication-only documentation changes do not alter that pairing.

The [v0.2.0-beta.1 four-player release](https://github.com/ATEMVEGETA/mGBA/releases/tag/wireless-adapter-v0.2.0-beta.1)
provides the exact application source at
`4884301060924ba4083de0a3e84cc9e461b0133c`, plus its own build/relink archive.
Its executable version is `0.11-codex/wireless-multiplayer-9162-488430106`.
The dependency libraries are unchanged; the accompanying dependency-source
archive is byte-identical to the earlier release's source payload described
below. BUILD-INFO and SHA-256 checksums distinguish each release's materials.

### Previous release and unchanged dependencies

The [v0.1.0-beta.1 release](https://github.com/ATEMVEGETA/mgba/releases/tag/wireless-adapter-v0.1.0-beta.1)
provides these materials alongside the compact runtime ZIP:

- The exact tracked mGBA source ZIP at
  `0441dab16deef170afa41aed170b65ca3b341623`. The executable identifies that
  commit as `0.11-codex/wireless-adapter-9160-0441dab16`.
- A dependency-source archive containing the complete signed MSYS2 Qt
  5.15.19-1 source package, its build recipe and patches; 25 exact-version
  MSYS2 source packages for the other linked support libraries; pinned FFmpeg,
  libepoxy and NV codec-header source archives; and associated build recipes,
  configuration and provenance records. These are source payloads, not just
  URLs or license notices.
- A build/relink archive containing application objects, the core library,
  exact link archives, generated/build records and a relink script. It permits
  replacing library archives and relinking with the matching MSYS2 UCRT64
  toolchain. Its README explains how to rebuild/relink; it is not needed just
  to run mGBA. Library source remains available separately in the source asset.
- SHA-256 checksums identifying all published assets.

Source-package signatures were verified against the installed MSYS2 keyring.
The Qt source's inner archive hash is
`173c2326dae138bbb0d98921e9d911e55c00163d93a6db29f294b5e19ff306ae`;
its PKGBUILD hash is
`0fe05984a674cb1f320d260bc7490192f733142fd93a67b540733edd7f805c8c`,
matching the binary package's `.BUILDINFO`. Each other package's exact version,
source URL, hash and signature report is recorded in the source manifest.

The v0.1.0-beta.1 runtime is unchanged from the approved window-size-fix build.
Only release-facing documentation, license information and checksums are
updated for publication. Later repository documentation commits are separate
from the immutable binary source revision above.

## RFU protocol references and credits

The RFU driver uses mGBA per-core mailboxes and local scheduling. Its protocol
work was informed and cross-checked against these independent projects and
the supplied VBALink material:

- [gpSP](https://github.com/davidgfnet/gpsp), by David Guillen Fandos and its
  contributors (GPL-2.0-or-later).
- [gba-link-connection](https://github.com/afska/gba-link-connection), by afska
  and contributors (MIT).
- VisualBoyAdvance/VBALink wireless implementation and timing behavior
  (GPL-2.0-or-later), including the user's supplied working source.

These references do not imply affiliation or endorsement. The present driver
is not presented as a verbatim VBA import, and no formal clean-room provenance
certification is claimed. Existing source attributions are retained. The
public fork excludes unrelated controls, Speed and e-Reader customizations
and private custom-branch history.

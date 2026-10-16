# Wireless Adapter for mGBA

An unofficial, experimental addition to the mGBA Qt frontend for local
Wireless Adapter communication between two emulated Game Boy Advance systems
on one computer. This is not an upstream mGBA release.

The original Link Cable mode remains available and is the default. Wireless
Adapter mode is a separate choice, not a replacement for the cable.

## Current status

This upstream-based port is a public beta. Broader gameplay compatibility of
the exact packaged frontend is still being established.

Earlier custom development builds received successful user reports for
Pokémon Jump, Dodrio Berry Picking, the Union Room, the Trade Room, and an
Aurora Ticket transfer. Those reports describe the earlier implementation;
they do not certify this new port, every game revision, or every save-file
combination. Wireless timing and compatibility remain experimental.

See [the release notes](WIRELESS-RELEASE-NOTES.md) for package status and known
limitations. Downloads, checksums and matching source are on the
[beta release page](https://github.com/ATEMVEGETA/mgba/releases/tag/wireless-adapter-v0.1.0-beta.1).
Build and packaging instructions are in [WIRELESS-BUILD.md](WIRELESS-BUILD.md).

## Before testing

- Keep your existing emulator installation and make separate backups of your
  in-game saves. Test the candidate in its own folder.
- Use compatible Wireless Adapter games and the intended save for each player.
  This package does not include games, ROMs, BIOS files, or saves.
- Use the normal mGBA input settings for each player. This port does not add a
  separate controller-mapping file or change upstream fast-forward controls.
- Run at normal speed while connecting and using a wireless room. Avoid
  fast-forwarding, rewinding, loading savestates, or pausing just one game
  during an active connection.

## Start a wireless session

1. Start this build of mGBA without opening a ROM. If games are already open,
   close all of them before selecting the link type.
2. Choose **File → GBA link type (next session) → Wireless Adapter**.
3. Open the first GBA game normally.
4. Choose **File → New multiplayer window**, then open the second GBA game
   in that window. Both windows must belong to the same mGBA process; opening
   a second independent copy of the program does not create this local link.
5. Start the wireless activity from the games' own menus. Follow their normal
   host/join instructions.

The setting applies to the next session, after every currently open game has
closed. Changing it while games are running does not switch their adapter
mid-session. The choice is stored in mGBA's normal configuration as
`gbaLinkMode`: `0` for Link Cable and `1` for Wireless Adapter. Editing the
configuration manually is not necessary.

Keep both games running and responsive. If one window is paused or closed,
the other game may report a lost connection.

## Use the Link Cable instead

Close the games in both multiplayer windows, select **File → GBA link type
(next session) → Link Cable**, then open the games again. Use this mode for
activities that require the cable. One multiplayer session cannot use both
adapter types at once.

Upstream input bindings, fast-forward behavior, and e-Reader behavior remain
in place. This wireless-only port does not include unrelated features from
earlier custom builds.

## Limitations and troubleshooting

- Wireless mode supports two local GBA players in the same application
  process. More players are not supported by this implementation.
- There is no LAN or internet transport, connection to another emulator,
  or bridge to a physical Wireless Adapter or real GBA.
- A savestate does not serialize the shared RFU session. Do not expect loading
  states in one or both windows to restore a live connection. Start a fresh
  multiplayer session instead.
- Successful discovery or entry to a room does not prove every later activity
  will work. Check the actual interaction, transfer, in-game save, disconnect,
  and closing behavior you intend to use.
- If a connection fails repeatedly, close both games and start a fresh
  session at normal speed. Do not overwrite your backup saves while
  investigating a failure.
- Diagnostic capture is not enabled by the normal release configuration.
  There is no requirement to create an extra logging or controls file.

For a useful bug report, include the exact build version, operating system,
game titles and revisions, selected link type, and steps to reproduce the
problem. Say whether one or both windows slow down, whether music is affected,
and whether the problem also occurs with only one game open. Screenshots are
helpful; do not attach ROMs, BIOS files, or private save files.

## Source and package verification

A published binary package must identify its exact source tag or commit and
provide the matching source archive and SHA-256 checksums. Keep the program
and dependency files from the same package together. Do not mix an executable
from one candidate with files from another.

The release page records the binary's exact source commit and SHA-256 checksums.
The source, dependency-source and build/relink archives are separate from the
small runtime ZIP; see [source and licensing information](WIRELESS-LICENSES.md).

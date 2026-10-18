# mGBA Wireless Adapter — beta release notes

## Unpublished expanded-radio prototype

Wireless environment capacity is separated from cable's four-console ABI.
This candidate permits 40 virtual adapters, with bounded rotating discovery
pages, globally distinct adapter identities and safe player/input/save claims
above the old four-window/32-bit-mask boundaries. Hardware data groups remain
one host plus four clients. Actual larger-room gameplay awaits user testing.
See [the prototype guide](WIRELESS-LARGE-ROOM.md). No new release was published.

## v0.2.0-beta.1 — four local players

This version extends the local Qt wireless session to four GBA windows. It
removes the two-player attachment restriction while retaining the existing
four-window, save-slot and input limits. The two-player rendezvous fast path
is retained for independent conversations; multi-client exchanges use the
existing fan-out transport with corrected synchronized completion handling,
including zero-byte RF packets. Late joiners adopt the host's relative clock
origin without resetting the other clients, so returning from a group to a
pair does not compare incompatible timing epochs.

Automated checks cover four-way discovery, two independent pairs, host-to-three-
client payloads and empty exchanges, pair/group membership transitions with
different surviving clients, and four-core attachment/teardown. They
do not replace gameplay testing. The user subsequently reported successful
four-player Pokémon Union Room play and authorized this release. That report
does not certify every title, revision, activity or save combination. No
ROM/BIOS/gameplay test was run by the coding agent during preparation.

The executables are byte-identical to the user-tested four-player candidate.
Executable source: `4884301060924ba4083de0a3e84cc9e461b0133c`.
Version: `0.11-codex/wireless-multiplayer-9162-488430106`.
The release tag points to this exact commit. Later documentation changes do
not alter the executable/source pairing. The prior release remains available.

## Previous published release

Status: v0.1.0-beta.1, published at the user's request. Wider compatibility
testing of this upstream-based package is still needed.

This is an unofficial experimental mGBA build, not an upstream release.

## What this adds

- A native **File → GBA link type (next session)** menu with **Link Cable**
  and **Wireless Adapter** choices.
- Local Wireless Adapter communication for two GBA games opened through
  **File → New multiplayer window** in the same application process.
- RFU command, packet, and serial-timing handling, plus bounded cooperative
  scheduling during an actual wireless exchange.
- A setting stored in mGBA's normal configuration. No separate controls file,
  external relay program, or terminal window is needed.

Link Cable remains the default. Link-type changes apply after all current
games close, not in the middle of an existing session. Original upstream
input controls, fast-forward behavior, and e-Reader behavior are retained.

Follow [the setup guide](WIRELESS-ADAPTER.md) before testing.

## Compatibility status

Earlier custom development builds received successful user reports for
Pokémon Jump, Dodrio Berry Picking, the Union Room, the Trade Room, and
Aurora Ticket delivery. These are encouraging results for the implementation,
not confirmation that the new upstream-based release package passes those
same scenarios.

Development tests exercise command and packet handling, physical serial IRQ
timing, retries, timeouts, and coordinator lifecycle behavior. They do not
replace manual tests of the exact packaged frontend, game interactions,
in-game saving, or closing both windows.

## Known limits

- v0.1.0-beta.1 supports two local GBA players; v0.2.0-beta.1 permits four
  windows in one process, with user-confirmed four-player Union Room play.
- No LAN, internet play, other-emulator transport, or real-hardware adapter
  connection.
- Shared RFU sessions are not serialized in savestates. Loading states or
  rewinding during an active connection is not a supported recovery method.
- Normal emulation speed is recommended during wireless activity. Pausing
  one game, fast-forwarding, or closing a peer can interrupt communication.
- Compatibility with every Wireless Adapter game, language, revision, or
  save-file combination is not established.
- Diagnostic logging is not activated in the normal release configuration.

## Package and source status

The window-sizing follow-up also corrects an upstream development regression:
an empty display no longer shrinks the startup window or prevents the frame-
size menu from working before a ROM is loaded. The normal 2× default, saved
window dimensions and existing frame-lock/fullscreen behavior are retained.
Size changes made without a ROM are saved on normal exit and restored the
next time the application opens, using the existing configuration mechanism.
This correction does not change Wireless Adapter communication.

The first compact candidate (`ed565e7f7`) crashes during native OpenGL startup
and must not be used. Its prebuilt static OpenGL dispatch library depended on
DLL initialization that does not run when linked into an executable. The
replacement uses mGBA's own libepoxy dependency fork with static PE TLS
initialization, and adds a production-entry/native OpenGL startup regression
test. This is a packaging dependency correction, not a change to RFU timing
or the game's wireless behavior.

The [release page](https://github.com/ATEMVEGETA/mgba/releases/tag/wireless-adapter-v0.1.0-beta.1)
provides the Windows x64 portable build, exact matching mGBA source ZIP,
dependency-source archive, build/relink materials and SHA-256 checksums.
The executable source is `0441dab16deef170afa41aed170b65ca3b341623`, version
`0.11-codex/wireless-adapter-9160-0441dab16`. Subsequent documentation-only
commits do not change that binary/source pairing.

ROMs, games, BIOS files and saves are not included. Upstream licenses and
dependency notices are retained. See [the licensing guide](WIRELESS-LICENSES.md)
for the GPL version 3 combined executable and original MPL source terms.

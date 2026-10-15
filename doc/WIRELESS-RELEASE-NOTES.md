# mGBA Wireless Adapter — beta release notes

Status: unpublished release candidate. The exact upstream-based package is
awaiting manual gameplay validation; a public release is not yet confirmed.

This is an unofficial experimental mGBA build, not an upstream release.

## What this adds

- A native **File → GBA link type (next session)** menu with **Link Cable**
  and **Wireless Adapter (experimental)** choices.
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

- Two local GBA players only, in one application process.
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

The planned release includes a Windows x64 portable Qt build, the source
archive corresponding to that exact binary, and SHA-256 checksums. ROMs,
games, BIOS files, and saves are not included. Upstream licenses and required
dependency notices must accompany the package.

Before publication, the release record must contain:

- The final version/tag and full source commit.
- The binary archive and matching source archive filenames.
- Their SHA-256 checksums.
- The results and remaining limitations of manual tests on that exact build.

These details are intentionally not filled with provisional values. This
draft does not announce a downloadable build or claim a completed release.

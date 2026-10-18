# Expanded local Wireless Adapter environment

This is a separate, unpublished prototype based on the user-tested four-player
release. It permits **up to 40 virtual adapters/windows** in Wireless Adapter
mode, while leaving Link Cable at four systems and Game Boy link at two.
Forty is an experimental safety ceiling, not a claim of unlimited resources
or confirmed 40-player gameplay. The released build and VEGETA installation
were not modified for this prototype.

## What the fifth game does

Each new multiplayer window runs its own emulated GBA and adapter. The fifth
and later games join the same virtual radio environment as earlier games;
they do not become a fifth cable slot or a new isolated room. There is no
need to automatically split the windows into batches of four.

The radio environment can contain multiple independent groups. An actual
Wireless Adapter data group still has **one host and up to four clients**,
subject to the game's own activity limits. The Union Room's discovery/display
logic is distinct from those active groups. The game decides which nearby
groups it displays; this does not modify ROM code or guarantee every nearby
independent trainer is visible simultaneously.

The fifth game must be opened with **File → New multiplayer window** in the
same application process. A separate mGBA executable process is not connected
by this implementation. There is no LAN/internet or real-hardware transport.

## Test steps

1. Use this package in its own directory with copies of saves. Do not overwrite
   the working installation or reuse a live connection's savestate.
2. Before loading games, choose **File → GBA link type (next session) →
   Wireless Adapter**.
3. Open the first compatible game, then create four additional multiplayer
   windows and load the other games there.
4. Test five-player Union Room discovery and interactions at normal speed.
   Then try eight windows if five behaves correctly.
5. Check that later-numbered games can find and interact with earlier ones;
   test independent conversations and leaving/rejoining or closing one peer.
6. Larger populations are exploratory and may be limited by CPU, graphics,
   memory, audio and the game's own room/group behavior. Start small.

Do not fast-forward, rewind, load states or pause a single peer during active
wireless communication. Changing Link Cable/Wireless selection applies only
after the active games close. Close spare windows before starting a cable
session; cable remains four-player. Keyboard/controller settings are the
normal upstream settings; the shared SDL input slot registry is enlarged to
avoid the former fifth-input fatal error.

## Implementation and verification scope

- `MAX_GBAS` remains four, preserving the original cable SIO register/driver
  ABI. The RFU environment has its own `GBA_RFU_MAX_ADAPTERS` capacity.
- Core mailboxes, registration, session isolation and cooperative service
  cover that environment; each console retains owner-local serial/timing
  state. No global forty-core execution barrier was added.
- Discovery still returns at most four hosts (28 words) per scan, compatible
  with the real adapter/ROM buffers. Successive scan cycles rotate through
  additional nearby hosts instead of overflowing a response.
- Adapter IDs distinguish global nodes; player/input/save claims no longer
  use a 32-bit bitmask. Same-ROM games retain distinct save suffixes even above
  player 32. Diagnostic capture is off by default.
- Advertisements coalesce to the latest queued snapshot from each sender.
  Replaceable discovery frames cannot crowd control traffic out indefinitely.
- Menu callbacks now disconnect with their QAction, fixing a reproduced stale
  callback crash after closing windows and changing the shared link setting.
  A full or mismatched session is rejected before a retained empty window
  starts another game; invalid save IDs do not fall back to the first save.

Automated protocol checks cover five/eight/forty-adapter discovery, twenty
independent pairs, five-member data/empty exchanges and rejection of an
illegal fifth child. Offscreen Qt checks cover window/input/capacity/save
claims and lifecycle; they do not execute a game CPU loop or use ROMs/BIOS.
The protocol suite has 166 cases in both deterministic and native-mutex
variants, the native-owner stress suite has 64, and the offscreen Qt fixture
has 10,173 checks. Actual fifth-game
Union Room behavior remains for the user's manual trial.

The prototype has not been uploaded to GitHub or installed into VEGETA.
No games, ROMs, BIOS, saves or personal controls are included.

## Protocol references

The [RFU protocol reference](https://github.com/afska/gba-link-connection/blob/master/docs/wireless_adapter.md)
documents four-host discovery pages, additional scan cycles and five-system
groups. [FireRed's Union Room constants](https://github.com/pret/pokefirered/blob/master/include/constants/union_room.h)
distinguish eight visible group leaders from four active child connections.

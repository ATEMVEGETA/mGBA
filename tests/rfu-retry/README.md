# Wireless Adapter protocol regression tests

These tests compile the actual RFU, SIO, and timing sources from this checkout.
They do not load a ROM, BIOS, save file, or emulator GUI. CPU IRQ delivery,
unrelated Game Boy Player initialization, and general logging are replaced by
test observation stubs; the RFU protocol itself is not mocked.

## Windows / PowerShell

Use a MinGW-w64 GCC installation on `PATH`, or pass its executable explicitly:

```powershell
./tests/rfu-retry/run.ps1
./tests/rfu-retry/run.ps1 -NativeMutex
./tests/rfu-retry/run-cooperative-native.ps1
```

Every runner accepts `-Compiler <path-to-gcc>`. Run the commands sequentially:
the regression variants share temporary diagnostic-file locations.

The default regression runner disables threading for deterministic protocol
checks. `-NativeMutex` repeats the same checks with real Windows mutexes and a
test-only guard that rejects recursively nested coordinator locks.

The separate Windows-native stress fixture exercises two concurrent adapter
owners and an independent watchdog service, including reset, mode changes,
callback removal, and detach races. Sleep/wake callbacks publish durable state;
the actual waits occur outside the coordinator mutex.

## Scope

The deterministic suite contains 160 cases, retaining the original 151 for command framing, serial
clock ownership, packet snapshots, retry and timeout bounds, session isolation,
prearmed readiness, completion-notice ownership, cooperative wakeups, lifecycle
retirement, and independent timer progress during an unoffered client WAIT.
Additional multiplayer cases cover four advertisers discovering all peers,
two independent synchronized pairs across different player assignments,
disconnection/reconnection, pair/group membership transitions with each
possible surviving client, and one host with three clients exchanging
distinct payloads or zero-byte notifications. They use the real serial
command/timing fixture; no game behavior is simulated or certified.
The native stress fixture contains 64 cases. Assertion counts in the native
stress fixture vary with thread interleaving.

Diagnostic-capture cases create uniquely named temporary `.log` files beside
the regression source and remove them on success. Failed tests can leave those
files for inspection. Executables and diagnostic files are ignored by Git.

Passing these tests does not establish hardware accuracy, gameplay
compatibility for every game, or frontend/audio behavior. Interactive testing
is still required for the Qt integration. No copyrighted game assets are
included or required by these fixtures.

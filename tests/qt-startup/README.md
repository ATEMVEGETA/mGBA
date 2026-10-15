# Production Windows startup regression

Run `./run.ps1` after building the compact static Qt Windows target. This
test complements the offscreen wireless-menu fixture: it exercises the
actual production `main.cpp.obj`, including its Unicode `wmain`, static
plugin imports, application initialization, default OpenGL display path,
first window, event loop, and normal application cleanup.

The runner uses `objcopy` to rename only the production object's `main` and
`wmain` symbols. Their implementations remain unchanged. A small wrapper
`wmain` sets a native four-second timer and calls the renamed production
`wmain`. It does not reimplement startup or select a software/offscreen
renderer. The timer runs inside that same test process and checks a visible
`QGBA::Window`, a default `DisplayGL`, no loaded game/controller, normal Qt
quit notification, and return code zero. Normal in-process Qt close/quit
calls end the fixture without external desktop input or UI automation.
If a nested initialization event loop receives the timer before a visible
application window exists, it retries briefly, with a 12-second readiness
limit inside the outer process timeout. It never accepts a software renderer
as a successful fallback.

All original Ninja target objects, library search paths, static/runtime
flags, and Unicode Windows entry-point flags are retained. The original
main object hash, compile/link responses, PE imports, report, and captured
output are kept in a fresh portable fixture directory. Runtime PATH contains
only Windows system directories, with the Windows Qt platform selected and
no external Qt plugin path. Existing emulator windows, settings, mappings,
ROMs, BIOS files, recordings, and saves are not used or changed.

The default external timeout is 20 seconds. Only the child process created
by this runner can be terminated on timeout; the runner never searches for
or terminates other emulator processes. A test failure or unsupported local
OpenGL environment is not silently changed into a software-renderer pass.

For an explicit negative control, `-ReproEpoxyArchive` can substitute a
preserved older `libepoxy.a` in the fixture's link response. It changes no
application source or normal build setting. This diagnostic mode is expected
to fail when pointed at the archive that caused the startup crash; it is not
a release configuration or a way to override dependencies in the application.
The wrapper suppresses Windows critical-error/crash dialogs for its own
process so this negative control can report a bounded failure unattended.

This is a startup/default-renderer/close regression, not game emulation,
multiplayer throughput, audio playback, recording, or full visual validation.
The linked fixture includes the production entry object, but is not byte-for-
byte identical to the released executable because it adds the timer wrapper.

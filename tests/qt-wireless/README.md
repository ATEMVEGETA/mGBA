# Qt wireless selection integration smoke

After building the Qt 5 Windows Ninja target, run `./run.ps1`. It reuses the
actual frontend objects and target compile/link settings, replacing only the
application entry point. The runner uses an isolated fresh directory beside
the build, a portable marker, and Qt's offscreen platform. No user AppData,
existing portable settings, ROM, BIOS, save or input mapping is modified.
The compiler defaults to `c++.exe` on PATH; `-Compiler` and
`-QtPlatformPluginPath` support other matching MinGW/Qt 5 installations.

Shared and static Qt 5 builds are supported. The runner preserves the target's
compile and link flags and library search paths (including static-runtime flags), replacing only its
entry point with the fixture's console `main`. For static Qt, `smoke.cpp`
imports the same plugins as the application plus the headless offscreen
platform plugin. The runner derives the configured Qt prefix from
`Qt5Core_DIR`/the link libraries, and adds `libqoffscreen.a` and its `.prl`
dependencies if the original executable target did not already link it.
`-QtStaticPrefix` and `-QtOffscreenPluginArchive` can override discovery for
another matching static installation. A missing static offscreen plugin or
dependency metadata is an explicit test-setup error, not a silent fallback
to a shared platform DLL.

The real `ConfigController`, `GBAApp` binding and offscreen `Window` menus
verify Cable as default, shared exclusive menu state, persisted selection,
and requested-versus-active adapter behavior through production attachment
and teardown. A test-only private member pointer observes GBAApp's existing
controller without adding a production API. Empty GBA cores initialize/reset
registers and timing only. Their inert thread fixtures execute no CPU loop;
normal frontend interrupt/stop/join cleanup still runs. The RFU watchdog is
the production worker, not a replacement.

Rooms of 5, 8 and 40 inert GBA cores exercise the real Wireless Adapter
attachment path. They verify distinct driver/thread-hook identities (including
the actual target thread), coordinator/global player IDs, registration sessions
and same-ROM save IDs. The 41st core must be rejected without consuming player
IDs, save claims or PIDs. Middle-slot detach/rejoin, including global ID 33,
must reuse the available IDs with a fresh registration session. Out-of-order
cleanup checks each survivor's driver, session, player ID and save suffix.
Explicit save suffixes 40 and 100, collision fallback, and freed-suffix reuse
are also checked independently of adapter capacity.

The real `newWindow()` gate rejects a fifth cable window, then admits it after
selecting wireless. Five offscreen windows confirm distinct input slots;
mode-aware capacity APIs verify the 40-window wireless limit and requested
versus active session behavior. Forty separate `InputController` instances
exercise all input claims and released-slot reuse above ID 31 without opening
forty display widgets. Full four-core GBA cable and two-core Game Boy sessions
retain their limits, reject excess attachments without consuming claims, and
clean up normally. The pre-start admission helper rejects full rooms and
mixed platforms and continues to use the active session's capacity when the
requested link type changes. This suite does not open forty frontend windows or run
forty emulated CPUs.

This is not gameplay, wireless protocol throughput, frontend CPU concurrency,
audio, visual-rendering, real-button or Playnite coverage. It supplements the
RFU protocol/native-owner suites and does not prove game compatibility. The
runner currently targets this Windows Qt 5/UCRT64 Ninja layout and retains
its generated fixture directory for inspection; it never deletes anything.

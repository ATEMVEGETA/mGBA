/* No-ROM integration check for the actual Qt config/menu/controller wiring.
 * This links the real frontend objects. Only the inert core-thread fixture is
 * synthetic: no CPU runLoop, ROM, BIOS, audio device or save file is used. */
#define SDL_MAIN_HANDLED
#include "ConfigController.h"
#include "GBAApp.h"
#include "Window.h"
#include "CoreController.h"

#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/core/thread.h>

#include <QDir>
#include <QFile>
#include <QMenuBar>
#include <QTemporaryDir>

#ifdef BUILD_SDL
#include <SDL.h>
#endif

#include <cstdio>
#include <cstdlib>

using namespace QGBA;

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
	std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (0)

/* Test-only pointer-to-member access observes the real GBAApp binding without
 * introducing a production accessor or replacing ConfigOption callbacks. */
struct MultiplayerMember {
	using type = MultiplayerController GBAApp::*;
	friend type testMember(MultiplayerMember);
};
template<class Tag, typename Tag::type Member> struct MemberAccess {
	friend typename Tag::type testMember(Tag) { return Member; }
};
template struct MemberAccess<MultiplayerMember, &GBAApp::m_multiplayer>;

static QAction* linkAction(Window* window, const QString& text) {
	for (QAction* top : window->menuBar()->actions()) {
		if (!top->menu()) continue;
		for (QAction* submenu : top->menu()->actions()) {
			if (!submenu->menu() || submenu->text() != QStringLiteral("GBA link type (next session)")) continue;
			for (QAction* action : submenu->menu()->actions()) if (action->text() == text) return action;
		}
	}
	return nullptr;
}

static THREAD_ENTRY inertThread(void*) { THREAD_EXIT(0); }

class InertCore {
public:
	explicit InertCore(ConfigController& config) {
		mCore* core = GBACoreCreate();
		CHECK(core && core->init(core));
		mCoreInitConfig(core, nullptr);
		mCoreLoadForeignConfig(core, config.config());
		core->reset(core); // Reset register/timing state only; executes no instructions.
		controller = std::make_unique<CoreController>(core);
		controller->setPath(QStringLiteral("wireless-fixture"), QString());
		mCoreThread* thread = controller->thread();
		thread->impl = static_cast<mCoreThreadInternal*>(std::calloc(1, sizeof(mCoreThreadInternal)));
		CHECK(thread->impl);
		thread->impl->state = mTHREAD_SHUTDOWN;
		thread->impl->core = core;
		MutexInit(&thread->impl->stateMutex);
		ConditionInit(&thread->impl->stateOnThreadCond);
		ConditionInit(&thread->impl->stateOffThreadCond);
		MutexInit(&thread->impl->sync.videoFrameMutex);
		ConditionInit(&thread->impl->sync.videoFrameAvailableCond);
		ConditionInit(&thread->impl->sync.videoFrameRequiredCond);
		MutexInit(&thread->impl->sync.audioBufferMutex);
		ConditionInit(&thread->impl->sync.audioRequiredCond);
		ThreadCreate(&thread->impl->thread, inertThread, nullptr);
		m_handle = thread->impl->thread;
		CHECK(m_handle);
	}
	~InertCore() {
		controller.reset(); // Production stop/join/deinit runs against the inert thread.
#ifdef _WIN32
		CloseHandle(m_handle);
#endif
	}
	GBASIODriver* driver() const { return static_cast<GBA*>(controller->thread()->core->board)->sio.driver; }
	std::unique_ptr<CoreController> controller;
private:
	Thread m_handle;
};

int main(int argc, char** argv) {
	CHECK(argc == 2);
	const QString isolated = QString::fromLocal8Bit(argv[1]);
	CHECK(QDir(isolated).exists());
	// The runner places the test executable beside a fresh portable.ini. Assert
	// isolation before constructing anything that can read/write configuration.
	CHECK(ConfigController::isPortable());
	CHECK(QDir(ConfigController::configDir()).canonicalPath() == QDir(isolated).canonicalPath());
#ifdef BUILD_SDL
	SDL_SetMainReady();
#endif
	ConfigController config;
	CHECK(config.getOption("gbaLinkMode").toInt() == 0);
	config.setOption("updateAutoCheck", 0);
	config.setOption("useDiscordPresence", 0);
	config.setQtOption("displayDriver", 0);
	config.setQtOption("audioDriver", -1);
	GBAApp app(argc, argv, &config);
	MultiplayerController& multiplayer = app.*testMember(MultiplayerMember{});
	CHECK(!multiplayer.wirelessEnabled());
	Window* first = app.newWindow();
	Window* second = app.newWindow();
	CHECK(first && second);
	QAction* cable[2] = {linkAction(first, QStringLiteral("Link Cable")), linkAction(second, QStringLiteral("Link Cable"))};
	QAction* wireless[2] = {linkAction(first, QStringLiteral("Wireless Adapter (experimental)")), linkAction(second, QStringLiteral("Wireless Adapter (experimental)"))};
	for (unsigned i = 0; i < 2; ++i) {
		std::printf("MENU window=%u cable=%p checked=%d wireless=%p checked=%d\n", i,
			static_cast<void*>(cable[i]), cable[i] ? cable[i]->isChecked() : -1,
			static_cast<void*>(wireless[i]), wireless[i] ? wireless[i]->isChecked() : -1);
		CHECK(cable[i] && wireless[i] && cable[i]->isChecked() && !wireless[i]->isChecked());
	}
	{
		InertCore one(config), two(config);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver() && one.driver()->driverId(one.driver()) == 0x6B636F4C);
		wireless[0]->trigger();
		CHECK(multiplayer.wirelessEnabled());
		CHECK(config.getOption("gbaLinkMode").toInt() == 1);
		for (unsigned i = 0; i < 2; ++i) CHECK(wireless[i]->isChecked() && !cable[i]->isChecked());
		// A requested change does not replace either active cable driver.
		CHECK(one.driver()->driverId(one.driver()) == 0x6B636F4C);
		CHECK(two.driver()->driverId(two.driver()) == 0x6B636F4C);
		multiplayer.detachGame(two.controller.get());
		multiplayer.detachGame(one.controller.get());
		CHECK(multiplayer.attached() == 0);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(one.driver() && one.driver()->driverId(one.driver()) == 0x52465500);
		cable[1]->trigger();
		CHECK(!multiplayer.wirelessEnabled());
		for (unsigned i = 0; i < 2; ++i) CHECK(cable[i]->isChecked() && !wireless[i]->isChecked());
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver()->driverId(one.driver()) == 0x52465500);
		CHECK(two.driver()->driverId(two.driver()) == 0x52465500);
		multiplayer.detachGame(one.controller.get());
		CHECK(two.driver()->driverId(two.driver()) == 0x52465500); // Survivor keeps its adapter.
		multiplayer.detachGame(two.controller.get());
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver()->driverId(one.driver()) == 0x6B636F4C);
		CHECK(two.driver()->driverId(two.driver()) == 0x6B636F4C);
		multiplayer.detachGame(two.controller.get());
		multiplayer.detachGame(one.controller.get());
	}
	wireless[1]->trigger();
	config.write();
	{
		ConfigController reloaded;
		CHECK(reloaded.getOption("gbaLinkMode").toInt() == 1);
	}
	delete second;
	delete first;
	CHECK(app.windows().isEmpty());
	CHECK(multiplayer.attached() == 0);
	std::printf("PASS Qt wireless config/menu/session integration (%u checks). No ROM, BIOS, CPU loop, audio or save used.\n", checks);
	return 0;
}

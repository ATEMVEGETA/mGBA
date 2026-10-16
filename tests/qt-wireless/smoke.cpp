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

#ifdef QT_STATIC
#include <QtPlugin>
// Keep the application's static imports when replacing its main object.
// Offscreen is an additional fixture-only import for headless execution.
#ifdef Q_OS_WIN
Q_IMPORT_PLUGIN(QJpegPlugin);
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin);
#ifdef BUILD_QT_MULTIMEDIA
Q_IMPORT_PLUGIN(QWindowsAudioPlugin);
Q_IMPORT_PLUGIN(DSServicePlugin);
#endif
#elif defined(Q_OS_MAC)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#ifdef BUILD_QT_MULTIMEDIA
Q_IMPORT_PLUGIN(CoreAudioPlugin);
Q_IMPORT_PLUGIN(AVFServicePlugin);
#endif
#elif defined(Q_OS_UNIX)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
Q_IMPORT_PLUGIN(QWaylandIntegrationPlugin);
#endif
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin);
#endif

#ifdef BUILD_SDL
#include <SDL.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <array>

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

struct ClaimedIdsMember {
	using type = int MultiplayerController::*;
	friend type testMember(ClaimedIdsMember);
};
struct NextPidMember {
	using type = int MultiplayerController::*;
	friend type testMember(NextPidMember);
};
struct ClaimedSavesMember {
	using type = QHash<QPair<QString, QString>, int> MultiplayerController::*;
	friend type testMember(ClaimedSavesMember);
};
template struct MemberAccess<ClaimedIdsMember, &MultiplayerController::m_claimedIds>;
template struct MemberAccess<NextPidMember, &MultiplayerController::m_nextPid>;
template struct MemberAccess<ClaimedSavesMember, &MultiplayerController::m_claimedSaves>;

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
	QAction* wireless[2] = {linkAction(first, QStringLiteral("Wireless Adapter")), linkAction(second, QStringLiteral("Wireless Adapter"))};
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
	{
		// The fixtures deliberately share a ROM path, so the production save-ID
		// allocator must reserve four different suffixes without opening saves.
		std::array<std::unique_ptr<InertCore>, MAX_GBAS> cores;
		std::array<GBASIORFUDriver*, MAX_GBAS> adapters{};
		std::array<uint32_t, MAX_GBAS> sessions{};
		std::array<int, MAX_GBAS> saveIds{};
		const int nextPidBefore = multiplayer.*testMember(NextPidMember{});
		CHECK(multiplayer.wirelessEnabled());
		CHECK(multiplayer.attached() == 0);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == 0);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
		for (unsigned i = 0; i < cores.size(); ++i) {
			cores[i] = std::make_unique<InertCore>(config);
			CHECK(multiplayer.attachGame(cores[i]->controller.get()));
			CHECK(multiplayer.attached() == static_cast<int>(i + 1));
			GBASIODriver* driver = cores[i]->driver();
			CHECK(driver && driver->driverId(driver) == 0x52465500);
			adapters[i] = reinterpret_cast<GBASIORFUDriver*>(driver);
			sessions[i] = adapters[i]->radioSession;
			saveIds[i] = multiplayer.saveId(cores[i]->controller.get());
			CHECK(adapters[i]->playerId == static_cast<int>(i));
			CHECK(adapters[i]->cooperativeUser);
			CHECK(sessions[i] != 0);
			CHECK(saveIds[i] == static_cast<int>(i + 1));
			CHECK(multiplayer.playerId(cores[i]->controller.get()) == static_cast<int>(i));
			for (unsigned j = 0; j < i; ++j) {
				CHECK(adapters[i] != adapters[j]);
				CHECK(adapters[i]->cooperativeUser != adapters[j]->cooperativeUser);
				CHECK(adapters[i]->coordinator == adapters[j]->coordinator);
				CHECK(sessions[i] != sessions[j]);
				CHECK(saveIds[i] != saveIds[j]);
			}
		}
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == (1 << MAX_GBAS) - 1);
		CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidBefore + MAX_GBAS);

		InertCore excess(config);
		GBASIODriver* excessDriver = excess.driver();
		const int claimedIdsBefore = multiplayer.*testMember(ClaimedIdsMember{});
		const int nextPidAtCapacity = multiplayer.*testMember(NextPidMember{});
		const auto claimedSavesBefore = multiplayer.*testMember(ClaimedSavesMember{});
		CHECK(!multiplayer.attachGame(excess.controller.get()));
		CHECK(multiplayer.attached() == MAX_GBAS);
		CHECK(excess.driver() == excessDriver);
		CHECK(multiplayer.playerId(excess.controller.get()) == -1);
		CHECK(multiplayer.saveId(excess.controller.get()) == -1);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == claimedIdsBefore);
		CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidAtCapacity);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == claimedSavesBefore);
		for (unsigned i = 0; i < cores.size(); ++i) {
			CHECK(cores[i]->driver() == &adapters[i]->d);
			CHECK(adapters[i]->radioSession == sessions[i]);
			CHECK(multiplayer.saveId(cores[i]->controller.get()) == saveIds[i]);
		}

		// Retiring a middle slot must leave every survivor attached. Rejoining
		// uses the available adapter/save slots but a fresh registration session.
		const unsigned middle = 1;
		const int middleId = adapters[middle]->playerId;
		multiplayer.detachGame(cores[middle]->controller.get());
		CHECK(multiplayer.attached() == MAX_GBAS - 1);
		CHECK(!cores[middle]->driver());
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == (claimedIdsBefore & ~(1 << middleId)));
		for (unsigned i = 0; i < cores.size(); ++i) {
			if (i == middle) continue;
			CHECK(cores[i]->driver() == &adapters[i]->d);
			CHECK(adapters[i]->radioSession == sessions[i]);
			CHECK(multiplayer.saveId(cores[i]->controller.get()) == saveIds[i]);
		}
		CHECK(multiplayer.attachGame(cores[middle]->controller.get()));
		CHECK(multiplayer.attached() == MAX_GBAS);
		GBASIODriver* rejoined = cores[middle]->driver();
		CHECK(rejoined && rejoined->driverId(rejoined) == 0x52465500);
		adapters[middle] = reinterpret_cast<GBASIORFUDriver*>(rejoined);
		CHECK(adapters[middle]->playerId == middleId);
		CHECK(adapters[middle]->radioSession != 0);
		CHECK(adapters[middle]->radioSession != sessions[middle]);
		CHECK(multiplayer.saveId(cores[middle]->controller.get()) == saveIds[middle]);
		sessions[middle] = adapters[middle]->radioSession;
		for (unsigned i = 0; i < cores.size(); ++i) {
			if (i == middle) continue;
			CHECK(adapters[middle]->radioSession != sessions[i]);
			CHECK(cores[i]->driver() == &adapters[i]->d);
		}
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == claimedIdsBefore);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == claimedSavesBefore);

		// Close the initial player first, then the other slots out of order.
		const unsigned closeOrder[MAX_GBAS] = {0, 2, 1, 3};
		for (unsigned i = 0; i < cores.size(); ++i) {
			multiplayer.detachGame(cores[closeOrder[i]]->controller.get());
			CHECK(!cores[closeOrder[i]]->driver());
			CHECK(multiplayer.attached() == static_cast<int>(cores.size() - i - 1));
			for (unsigned j = i + 1; j < cores.size(); ++j) {
				const unsigned survivor = closeOrder[j];
				CHECK(cores[survivor]->driver() == &adapters[survivor]->d);
				CHECK(adapters[survivor]->radioSession == sessions[survivor]);
			}
		}
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == 0);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
		CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidBefore + MAX_GBAS + 1);
	}
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
